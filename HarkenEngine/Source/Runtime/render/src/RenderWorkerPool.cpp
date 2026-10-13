#include "harken/render/RenderWorkerPool.h"

#include "harken/core/AllocGuard.h"
#include "harken/core/CpuClock.h"
#include "harken/core/FloatMode.h"
#include "harken/render/RenderWorkerPoolWakeConfig.h"

#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
#include "harken/core/RtProbe.h"
#endif

#include <cassert>

#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
#include <atomic>
#endif

#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
#include <mach/mach_init.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#endif

namespace harken {

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace {
struct OriginalSourceWorkerTestGates {
    std::atomic<const RenderWorkerPool*> receiver{nullptr};
    original_source_worker_test::CoordinatorGate coordinator = nullptr;
    original_source_worker_test::ClaimGate claimed = nullptr;
    void* context = nullptr;
};
OriginalSourceWorkerTestGates g_originalSourceWorkerTestGates;
void ClearOriginalSourceWorkerTestGatesAfterJoin(const RenderWorkerPool& pool) noexcept {
    auto& gates = g_originalSourceWorkerTestGates;
    if (gates.receiver.load(std::memory_order_acquire) != &pool) return;
    gates.receiver.store(nullptr, std::memory_order_release);
    gates.coordinator = nullptr;
    gates.claimed = nullptr;
    gates.context = nullptr;
}
void EnterOriginalSourceWorkerClaimGate(const RenderWorkerPool& pool,
    std::uint64_t generation, std::uint32_t job) noexcept {
    const auto& gates = g_originalSourceWorkerTestGates;
    if (gates.receiver.load(std::memory_order_acquire) == &pool && gates.claimed != nullptr)
        gates.claimed(gates.context, generation, job);
}
void EnterOriginalSourceCoordinatorGate(const RenderWorkerPool& pool,
    std::uint64_t generation) noexcept {
    const auto& gates = g_originalSourceWorkerTestGates;
    if (gates.receiver.load(std::memory_order_acquire) == &pool && gates.coordinator != nullptr)
        gates.coordinator(gates.context, generation);
}
} // namespace
namespace original_source_worker_test {
void Configure(RenderWorkerPool& pool, CoordinatorGate coordinator,
    ClaimGate claimed, void* context) noexcept {
    // At most one quiescent native pool is selected for workload delay. Batch
    // publication supplies the existing release/acquire visibility to workers.
    auto& gates = g_originalSourceWorkerTestGates;
    const auto* receiver = gates.receiver.load(std::memory_order_acquire);
    if (receiver != nullptr && receiver != &pool) {
        assert(false && "Only the selected native pool may configure this test gate");
        return;
    }
    if (coordinator == nullptr && claimed == nullptr) {
        ClearOriginalSourceWorkerTestGatesAfterJoin(pool);
        return;
    }
    gates.coordinator = coordinator;
    gates.claimed = claimed;
    gates.context = context;
    gates.receiver.store(&pool, std::memory_order_release);
}
} // namespace original_source_worker_test
#endif

#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
namespace {

struct TestGateState {
    std::atomic<std::uint32_t> expected{0};
    std::atomic<std::uint32_t> entered{0};
    std::atomic<bool> armed{false};
    std::atomic<bool> released{true};
    std::atomic<bool> unordered{false};
};

TestGateState g_beforeFirstEpochLoadGate;
TestGateState g_beforeWaitGate;
TestGateState g_beforeJobFunctionGate;
std::atomic<std::uint64_t> g_testNotificationCount{0};
std::atomic<bool> g_testFailNextNotification{false};
constexpr std::uint32_t kNoWithheldWorker = UINT32_MAX;
std::atomic<std::uint32_t> g_testWithholdNextWorker{kNoWithheldWorker};
std::atomic<std::uint32_t> g_testFailStartBeforeWorker{kNoWithheldWorker};

struct ForcedStartFailure final {};

TestGateState& GateState(render_worker_pool_test::TestGatePoint point) {
    switch (point) {
    case render_worker_pool_test::TestGatePoint::BeforeFirstEpochLoad:
        return g_beforeFirstEpochLoadGate;
    case render_worker_pool_test::TestGatePoint::BeforeJobFunction:
        return g_beforeJobFunctionGate;
    case render_worker_pool_test::TestGatePoint::BeforeWait:
        break;
    }
    return g_beforeWaitGate;
}

void ArmTestGate(TestGateState& gate, std::uint32_t expectedWorkers,
                 bool unordered) {
    gate.entered.store(0, std::memory_order_relaxed);
    gate.expected.store(expectedWorkers, std::memory_order_relaxed);
    gate.released.store(false, std::memory_order_relaxed);
    gate.unordered.store(unordered, std::memory_order_relaxed);
    gate.armed.store(true, std::memory_order_release);
}

void EnterTestGate(render_worker_pool_test::TestGatePoint point) {
    auto& gate = GateState(point);
    if (!gate.armed.load(std::memory_order_acquire)) {
        return;
    }
    // In unordered mode neither the arrival nor the release observation carries
    // an ordering constraint, so the gate contributes no happens-before edge of
    // its own to the code it is holding.
    if (gate.unordered.load(std::memory_order_relaxed)) {
        gate.entered.fetch_add(1, std::memory_order_relaxed);
        while (!gate.released.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        return;
    }
    gate.entered.fetch_add(1, std::memory_order_acq_rel);
    while (!gate.released.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

} // namespace

namespace render_worker_pool_test {

void ArmBeforeFirstEpochLoadGate(std::uint32_t expectedWorkers) {
    ArmTestGate(g_beforeFirstEpochLoadGate, expectedWorkers,
                /*unordered=*/false);
}

void ArmBeforeWaitGate(std::uint32_t expectedWorkers) {
    ArmTestGate(g_beforeWaitGate, expectedWorkers, /*unordered=*/false);
}

void ArmBeforeJobFunctionGate(std::uint32_t expectedWorkers,
                              TestGateOrdering ordering) {
    ArmTestGate(g_beforeJobFunctionGate, expectedWorkers,
                ordering == TestGateOrdering::Unordered);
}

std::uint32_t GateEnteredCount(
    render_worker_pool_test::TestGatePoint point) {
    auto& gate = GateState(point);
    if (gate.unordered.load(std::memory_order_relaxed)) {
        return gate.entered.load(std::memory_order_relaxed);
    }
    return gate.entered.load(std::memory_order_acquire);
}

void ReleaseGate(render_worker_pool_test::TestGatePoint point) {
    auto& gate = GateState(point);
    if (gate.unordered.load(std::memory_order_relaxed)) {
        gate.released.store(true, std::memory_order_relaxed);
        gate.armed.store(false, std::memory_order_relaxed);
        return;
    }
    gate.released.store(true, std::memory_order_release);
    gate.armed.store(false, std::memory_order_release);
}

std::uint64_t NotificationCount() {
    return g_testNotificationCount.load(std::memory_order_acquire);
}

void FailNextWakeNotification() {
    g_testFailNextNotification.store(true, std::memory_order_release);
}

void WithholdNextWakeNotificationForWorker(std::uint32_t workerId) {
    g_testWithholdNextWorker.store(workerId, std::memory_order_release);
}

void FailNextStartBeforeWorker(std::uint32_t workerId) {
    g_testFailStartBeforeWorker.store(workerId, std::memory_order_release);
}

bool IsSerialFallback(const RenderWorkerPool& pool) {
    return pool.m_serialFallback;
}

std::uint32_t JobFaultCount(const RenderWorkerPool& pool) {
    return pool.m_jobFaultCount.load(std::memory_order_acquire);
}

bool HasCleanRestartState(const RenderWorkerPool& pool) {
    if (pool.m_epoch.load(std::memory_order_acquire) != 0 ||
        pool.m_stop.load(std::memory_order_acquire) ||
        pool.m_wakeFault.load(std::memory_order_acquire) ||
        pool.m_jobFault.load(std::memory_order_acquire) ||
        pool.m_runActive.load(std::memory_order_acquire) ||
        pool.m_serialFallback || pool.m_abandonedGeneration != 0 ||
        pool.m_abandonedJobCount != 0 ||
        pool.m_dispatchState.load(std::memory_order_acquire) !=
            RenderWorkerPool::DispatchState::Disabled ||
        pool.m_domainBound || pool.m_boundDomain.generation != 0 ||
        pool.m_boundDomain.context != nullptr ||
        pool.m_boundDomain.ops.join != nullptr ||
        pool.m_boundDomain.ops.leave != nullptr ||
        pool.m_batches[0].command != RenderWorkerPool::Command::None ||
        pool.m_batches[1].command != RenderWorkerPool::Command::None ||
        pool.m_batches[0].fn != nullptr || pool.m_batches[1].fn != nullptr ||
        pool.m_batches[0].userData != nullptr ||
        pool.m_batches[1].userData != nullptr ||
        pool.m_batches[0].jobCount != 0 ||
        pool.m_batches[1].jobCount != 0 ||
        pool.m_nextJob[0].load(std::memory_order_acquire) != 0 ||
        pool.m_nextJob[1].load(std::memory_order_acquire) != 0 ||
        pool.m_workersFinished[0].load(std::memory_order_acquire) != 0 ||
        pool.m_workersFinished[1].load(std::memory_order_acquire) != 0 ||
        pool.m_jobsCompleted[0].load(std::memory_order_acquire) != 0 ||
        pool.m_jobsCompleted[1].load(std::memory_order_acquire) != 0 ||
        pool.m_controlDomain.generation != 0 ||
        pool.m_controlDomain.context != nullptr ||
        pool.m_controlDomain.ops.join != nullptr ||
        pool.m_controlDomain.ops.leave != nullptr ||
        pool.m_controlFinished.load(std::memory_order_acquire) != 0 ||
        pool.m_controlFailures.load(std::memory_order_acquire) != 0) {
        return false;
    }
    for (std::uint64_t generation :
         pool.m_workerControlAckGenerations) {
        if (generation != 0) {
            return false;
        }
    }
    for (std::uint64_t generation : pool.m_workerDomainGenerations) {
        if (generation != 0) {
            return false;
        }
    }
    return true;
}

void NotifyWithoutEpochChange(RenderWorkerPool& pool) {
    pool.NotifyEpochChanged();
}

} // namespace render_worker_pool_test
#endif

RenderWorkerPool::~RenderWorkerPool() {
    Stop();
}

bool RenderWorkerPool::Start(std::uint32_t workerCount,
                             std::uint32_t maxJobs) noexcept {
#ifndef NDEBUG
    assert(!m_startedOnce);
#endif
    if (m_startedOnce) {
        return false;
    }
    m_startedOnce = true;
    m_configuredThreadCount = workerCount;
    m_configuredMaxJobs = maxJobs;
    m_jobFaultCount.store(0, std::memory_order_relaxed);
    return CreateThreadSet(workerCount);
}

bool RenderWorkerPool::EnsureReady() noexcept {
#ifndef NDEBUG
    assert(m_startedOnce);
    assert(!m_runActive.load(std::memory_order_acquire));
#endif
    if (!m_startedOnce ||
        m_runActive.load(std::memory_order_acquire) || m_domainBound ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled) {
        return false;
    }
    if (HasReadyThreadSet()) {
        return true;
    }

    // No callback or domain may be live at this lifecycle boundary. Stop
    // therefore owns the complete quiescence transaction for a degraded set,
    // including worker-local Leave and wake-fault rescue before join.
    Stop();
    return CreateThreadSet(m_configuredThreadCount);
}

bool RenderWorkerPool::HasReadyThreadSet() const noexcept {
    if (m_configuredThreadCount == 0) {
        return m_threads.empty() && m_threadCount == 0 &&
               !m_stop.load(std::memory_order_acquire) &&
               !m_wakeFault.load(std::memory_order_acquire) &&
               !m_jobFault.load(std::memory_order_acquire) &&
               !m_serialFallback;
    }
    if (m_threadCount != m_configuredThreadCount ||
        m_threads.size() != m_configuredThreadCount ||
        m_threadsReady.load(std::memory_order_acquire) !=
            m_configuredThreadCount ||
        m_threadStartupFaults.load(std::memory_order_acquire) != 0 ||
        m_stop.load(std::memory_order_acquire) ||
        m_wakeFault.load(std::memory_order_acquire) ||
        m_jobFault.load(std::memory_order_acquire) || m_serialFallback ||
        m_workerBusyTicks[0].size() != m_configuredThreadCount ||
        m_workerBusyTicks[1].size() != m_configuredThreadCount ||
        m_jobState.size() != m_configuredMaxJobs ||
        m_jobDoneAtAbandonment.size() != m_configuredMaxJobs ||
        m_abandonedGeneration != 0 ||
        m_workerControlAckGenerations.size() !=
            m_configuredThreadCount ||
        m_workerDomainGenerations.size() != m_configuredThreadCount) {
        return false;
    }
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE || HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
    if (m_wakeSemaphores.size() != m_configuredThreadCount) {
        return false;
    }
#endif
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
    if (m_wakePrimitivesDestroyed) {
        return false;
    }
#endif
    return true;
}

bool RenderWorkerPool::CreateThreadSet(
    std::uint32_t workerCount) noexcept {
#ifndef NDEBUG
    assert(m_threads.empty() && m_threadCount == 0);
    assert(!m_domainBound);
    assert(!m_runActive.load(std::memory_order_acquire));
    assert(m_dispatchState.load(std::memory_order_acquire) ==
           DispatchState::Disabled);
#endif
    if (!m_threads.empty() || m_threadCount != 0 || m_domainBound ||
        m_runActive.load(std::memory_order_acquire) ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled) {
        return false;
    }

    // A stopped set leaves a non-zero epoch and may leave its last control or
    // batch command published. Reset every descriptor before any replacement
    // worker can publish readiness and perform its first epoch load.
    DestroyWakePrimitives();
    m_epoch.store(0, std::memory_order_relaxed);
    m_stop.store(false, std::memory_order_relaxed);
    m_wakeFault.store(false, std::memory_order_relaxed);
    m_jobFault.store(false, std::memory_order_relaxed);
    m_runActive.store(false, std::memory_order_relaxed);
    m_threadsReady.store(0, std::memory_order_relaxed);
    m_threadStartupFaults.store(0, std::memory_order_relaxed);
    m_dispatchState.store(DispatchState::Disabled,
                          std::memory_order_relaxed);
    m_serialFallback = false;
    m_domainBound = false;
    m_boundDomain = RealtimeWorkerDomain{};
    for (std::uint32_t slot = 0; slot < kGenerationSlots; ++slot) {
        m_batches[slot] = BatchDescriptor{};
        m_nextJob[slot].store(0, std::memory_order_relaxed);
        m_workersFinished[slot].store(0, std::memory_order_relaxed);
        m_jobsCompleted[slot].store(0, std::memory_order_relaxed);
    }
    m_controlDomain = RealtimeWorkerDomain{};
    m_controlFinished.store(0, std::memory_order_relaxed);
    m_controlFailures.store(0, std::memory_order_relaxed);
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    m_probeBatchSink = nullptr;
#endif
    m_workerBusyTicks[0].clear();
    m_workerBusyTicks[1].clear();
    m_jobState.clear();
    m_jobDoneAtAbandonment.clear();
    m_abandonedGeneration = 0;
    m_abandonedJobCount = 0;
    m_workerControlAckGenerations.clear();
    m_workerDomainGenerations.clear();
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
    const std::uint32_t failStartBeforeWorker =
        g_testFailStartBeforeWorker.exchange(kNoWithheldWorker,
                                              std::memory_order_acq_rel);
#endif
    if (workerCount == 0) {
        return true;
    }
    m_threadCount = workerCount;
    try {
        m_workerBusyTicks[0].assign(workerCount, 0);
        m_workerBusyTicks[1].assign(workerCount, 0);
        // One word per job, sized from the frozen bound so the audio thread
        // never grows it. Zero is generation 0 / Unclaimed, which no live
        // generation ever equals (the first published batch is generation 1).
        m_jobState = std::vector<std::atomic<std::uint64_t>>(
            m_configuredMaxJobs);
        for (std::atomic<std::uint64_t>& word : m_jobState) {
            word.store(0, std::memory_order_relaxed);
        }
        // The classification snapshot's storage, sized from the same frozen
        // bound. Its contents are meaningless until an abandonment writes them.
        m_jobDoneAtAbandonment.assign(m_configuredMaxJobs, 0);
        m_workerControlAckGenerations.assign(workerCount, 0);
        m_workerDomainGenerations.assign(workerCount, 0);
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
        m_wakePrimitivesDestroyed = false;
        m_wakeSemaphores.reserve(workerCount);
        for (std::uint32_t id = 0; id < workerCount; ++id) {
            semaphore_t semaphore = MACH_PORT_NULL;
            const kern_return_t result =
                semaphore_create(mach_task_self(), &semaphore,
                                 SYNC_POLICY_FIFO, 0);
            if (result != KERN_SUCCESS) {
                for (std::uint32_t created : m_wakeSemaphores) {
                    semaphore_destroy(mach_task_self(),
                                      static_cast<semaphore_t>(created));
                }
                m_wakeSemaphores.clear();
                m_workerBusyTicks[0].clear();
                m_workerBusyTicks[1].clear();
                m_jobState.clear();
                m_jobDoneAtAbandonment.clear();
                m_workerControlAckGenerations.clear();
                m_workerDomainGenerations.clear();
                m_threadCount = 0;
                return false;
            }
            m_wakeSemaphores.push_back(static_cast<std::uint32_t>(semaphore));
        }
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
        m_wakeSemaphores.reserve(workerCount);
        for (std::uint32_t id = 0; id < workerCount; ++id) {
            m_wakeSemaphores.push_back(std::make_unique<CountingSemaphore>(0));
        }
#endif
        m_threads.reserve(workerCount);
        for (std::uint32_t id = 0; id < workerCount; ++id) {
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
            if (id == failStartBeforeWorker) {
                throw ForcedStartFailure{};
            }
#endif
            m_threads.emplace_back([this, id] { WorkerMain(id); });
        }
        while (m_threadsReady.load(std::memory_order_acquire) != workerCount) {
            std::this_thread::yield();
        }
        if (m_threadStartupFaults.load(std::memory_order_acquire) != 0) {
            Stop();
            return false;
        }
    } catch (...) {
        if (!m_threads.empty()) {
            m_stop.store(true, std::memory_order_release);
            m_epoch.fetch_add(1, std::memory_order_release);
            NotifyEpochChanged();
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
            if (m_wakeFault.load(std::memory_order_acquire)) {
                DestroyWakePrimitives();
            }
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
            if (m_wakeFault.load(std::memory_order_acquire)) {
                for (auto& semaphore : m_wakeSemaphores) {
                    semaphore->release();
                }
            }
#endif
            for (std::thread& thread : m_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            m_threads.clear();
        }
        DestroyWakePrimitives();
        m_workerBusyTicks[0].clear();
        m_workerBusyTicks[1].clear();
        m_jobState.clear();
        m_jobDoneAtAbandonment.clear();
        m_workerControlAckGenerations.clear();
        m_workerDomainGenerations.clear();
        m_threadCount = 0;
        return false;
    }
    return true;
}

bool RenderWorkerPool::BindRealtimeDomain(
    const RealtimeWorkerDomain& domain) {
#ifndef NDEBUG
    assert(!m_runActive.load(std::memory_order_acquire));
#endif
    if (m_configuredThreadCount == 0) {
        return true;
    }
    if (m_threadCount == 0) {
        return false;
    }
    if (m_threads.size() != m_threadCount || m_domainBound ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled ||
        domain.generation == 0 || domain.ops.join == nullptr ||
        domain.ops.leave == nullptr ||
        m_threadsReady.load(std::memory_order_acquire) != m_threadCount ||
        m_threadStartupFaults.load(std::memory_order_acquire) != 0 ||
        m_wakeFault.load(std::memory_order_acquire) ||
        m_jobFault.load(std::memory_order_acquire) || m_serialFallback) {
        return false;
    }

    if (!DispatchDomainControl(true, domain)) {
        Stop();
        return false;
    }

    bool allJoined =
        m_controlFailures.load(std::memory_order_acquire) == 0;
    for (std::uint32_t id = 0; id < m_threadCount; ++id) {
        if (m_workerDomainGenerations[id] != domain.generation) {
            allJoined = false;
        }
    }
    if (!allJoined) {
        // Join failure is transactional: a distinct control round runs Leave
        // on each worker that successfully joined this generation. Resetting
        // the acknowledgement barrier prevents a retry from consuming a
        // completion left by this failed attempt.
        if (!DispatchDomainControl(false, domain)) {
            Stop();
        }
        return false;
    }

    m_boundDomain = domain;
    m_domainBound = true;
    return true;
}

bool RenderWorkerPool::UnbindRealtimeDomain() {
#ifndef NDEBUG
    assert(!m_runActive.load(std::memory_order_acquire));
#endif
    Disable();
    if (m_runActive.load(std::memory_order_acquire) ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled) {
        // The callback owner must synchronously stop the callback before
        // unbinding. A batch that won the Disable race is still entitled to
        // finish; do not overwrite its command or tear down its domain.
        return false;
    }
    if (!m_domainBound) {
        return true;
    }
    const RealtimeWorkerDomain domain = m_boundDomain;
    if (!DispatchDomainControl(false, domain)) {
        // Stop retains the descriptor until every worker has exited. WorkerMain
        // runs Leave for any local membership before returning, including the
        // wake-fault teardown path.
        Stop();
        return false;
    }
    m_domainBound = false;
    m_boundDomain = RealtimeWorkerDomain{};
    return true;
}

bool RenderWorkerPool::EnableRealtime() noexcept {
#ifndef NDEBUG
    assert(!m_runActive.load(std::memory_order_acquire));
#endif
    if (m_configuredThreadCount == 0) {
        return true;
    }
    if (m_threadCount == 0) {
        return false;
    }
    if (!m_domainBound ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled ||
        m_threadsReady.load(std::memory_order_acquire) != m_threadCount ||
        m_wakeFault.load(std::memory_order_acquire) ||
        m_jobFault.load(std::memory_order_acquire) || m_serialFallback) {
        return false;
    }
    for (std::uint32_t id = 0; id < m_threadCount; ++id) {
        if (m_workerDomainGenerations[id] != m_boundDomain.generation) {
            return false;
        }
    }
    m_dispatchState.store(DispatchState::Enabled,
                          std::memory_order_release);
    return true;
}

bool RenderWorkerPool::EnableOffline() noexcept {
#ifndef NDEBUG
    assert(!m_runActive.load(std::memory_order_acquire));
#endif
    if (m_configuredThreadCount == 0) {
        return true;
    }
    if (m_threadCount == 0) {
        return false;
    }
    if (m_domainBound ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled ||
        m_threadsReady.load(std::memory_order_acquire) != m_threadCount ||
        m_threadStartupFaults.load(std::memory_order_acquire) != 0 ||
        m_wakeFault.load(std::memory_order_acquire) ||
        m_jobFault.load(std::memory_order_acquire) || m_serialFallback) {
        return false;
    }
    m_dispatchState.store(DispatchState::Enabled,
                          std::memory_order_release);
    return true;
}

void RenderWorkerPool::Disable() noexcept {
    DispatchState state = m_dispatchState.load(std::memory_order_acquire);
    for (;;) {
        if (state == DispatchState::Disabled ||
            state == DispatchState::BatchActiveDisablePending) {
            return;
        }
        const DispatchState desired =
            state == DispatchState::BatchActive
                ? DispatchState::BatchActiveDisablePending
                : DispatchState::Disabled;
        if (m_dispatchState.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

bool RenderWorkerPool::TryReserveBatch() noexcept {
    DispatchState expected = DispatchState::Enabled;
    if (!m_dispatchState.compare_exchange_strong(
            expected, DispatchState::BatchActive,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    m_runActive.store(true, std::memory_order_release);
    return true;
}

void RenderWorkerPool::FinishReservedBatch() noexcept {
    DispatchState state = m_dispatchState.load(std::memory_order_acquire);
    for (;;) {
        DispatchState desired = DispatchState::Disabled;
        if (state == DispatchState::BatchActive) {
            desired = DispatchState::Enabled;
        } else if (state !=
                   DispatchState::BatchActiveDisablePending) {
#ifndef NDEBUG
            assert(false && "render-worker batch reservation is not active");
#endif
            break;
        }
        if (m_dispatchState.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
    m_runActive.store(false, std::memory_order_release);
}

void RenderWorkerPool::CancelReservedBatch() noexcept {
    FinishReservedBatch();
}

bool RenderWorkerPool::DispatchDomainControl(
    bool bind, const RealtimeWorkerDomain& domain) {
#ifndef NDEBUG
    assert(!m_runActive.load(std::memory_order_acquire));
    assert(m_dispatchState.load(std::memory_order_acquire) ==
           DispatchState::Disabled);
#endif
    if (m_runActive.load(std::memory_order_acquire) ||
        m_dispatchState.load(std::memory_order_acquire) !=
            DispatchState::Disabled ||
        m_threads.size() != m_threadCount ||
        m_wakeFault.load(std::memory_order_acquire)) {
        return false;
    }

    m_controlDomain = domain;
    m_controlFinished.store(0, std::memory_order_relaxed);
    m_controlFailures.store(0, std::memory_order_relaxed);
    // A control round is published through the same epoch bump as a batch, so
    // it writes the copy the generation it is about to create will read.
    const std::uint32_t controlSlot =
        SlotOf(m_epoch.load(std::memory_order_relaxed) + 1);
    m_batches[controlSlot].command =
        bind ? Command::BindDomain : Command::LeaveDomain;
    m_epoch.fetch_add(1, std::memory_order_release);
    NotifyEpochChanged();
    if (m_wakeFault.load(std::memory_order_acquire)) {
        return false;
    }
    while (m_controlFinished.load(std::memory_order_acquire) !=
           m_threadCount) {
        if (m_wakeFault.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::yield();
    }

    for (std::uint32_t id = 0; id < m_threadCount; ++id) {
        if (m_workerControlAckGenerations[id] != domain.generation) {
            return false;
        }
    }
    if (!bind &&
        m_controlFailures.load(std::memory_order_acquire) != 0) {
        return false;
    }
    m_batches[controlSlot].command = Command::None;
    return true;
}

bool RenderWorkerPool::LeaveJoinedDomain(
    const RealtimeWorkerDomain& domain, std::uint32_t id) noexcept {
    if (domain.ops.leave != nullptr && domain.generation != 0) {
        return domain.ops.leave(domain.context, id, domain.generation);
    }
    return false;
}

void RenderWorkerPool::Stop() {
    if (m_threads.empty()) {
        DestroyWakePrimitives();
        m_dispatchState.store(DispatchState::Disabled,
                              std::memory_order_release);
        m_domainBound = false;
        m_boundDomain = RealtimeWorkerDomain{};
        m_threadCount = 0;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
        ClearOriginalSourceWorkerTestGatesAfterJoin(*this);
#endif
        return;
    }
#ifndef NDEBUG
    // Stop must not overlap an in-flight Run; the engine owns this lifecycle
    // ordering and this assertion keeps that precondition visible in debug.
    assert(!m_runActive.load(std::memory_order_acquire));
#endif
    Disable();
    if (m_domainBound &&
        !m_wakeFault.load(std::memory_order_acquire)) {
        if (DispatchDomainControl(false, m_boundDomain)) {
            m_domainBound = false;
            m_boundDomain = RealtimeWorkerDomain{};
        }
    }
    // Publish the stop and unpark every worker with the same epoch bump the
    // batch release uses; a worker sees stop the next time its wait returns.
    m_stop.store(true, std::memory_order_release);
    m_epoch.fetch_add(1, std::memory_order_release);
    NotifyEpochChanged();
    const bool wakeFault = m_wakeFault.load(std::memory_order_acquire);
    const bool jobFault = m_jobFault.load(std::memory_order_acquire);
    if (wakeFault || jobFault) {
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
        if (wakeFault) {
            // Destroying a Mach semaphore wakes a current waiter with an
            // error. A worker racing into semaphore_wait after this point
            // receives the same invalid-port error, so every worker leaves
            // before the join.
            DestroyWakePrimitives();
        }
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
        if (wakeFault) {
            // A counting semaphore cannot be destroyed while a waiter may
            // still access it. Supply a second token to every worker; unlike
            // the Mach notification, release cannot report a per-worker wake
            // failure.
            for (auto& semaphore : m_wakeSemaphores) {
                semaphore->release();
            }
        }
#endif
    }
    for (std::thread& t : m_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_threads.clear();
    DestroyWakePrimitives();
    m_domainBound = false;
    m_boundDomain = RealtimeWorkerDomain{};
    m_workerBusyTicks[0].clear();
    m_workerBusyTicks[1].clear();
    m_jobState.clear();
    m_jobDoneAtAbandonment.clear();
    m_workerControlAckGenerations.clear();
    m_workerDomainGenerations.clear();
    m_threadCount = 0;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    // No worker can retain or reread the workload gate after the real join.
    ClearOriginalSourceWorkerTestGatesAfterJoin(*this);
#endif
}

void RenderWorkerPool::NotifyEpochChanged() {
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
    g_testNotificationCount.fetch_add(1, std::memory_order_release);
    const std::uint32_t withheldWorker =
        g_testWithholdNextWorker.exchange(kNoWithheldWorker,
                                          std::memory_order_acq_rel);
    if (withheldWorker != kNoWithheldWorker) {
        m_wakeFault.store(true, std::memory_order_release);
        Disable();
    }
    if (g_testFailNextNotification.exchange(false,
                                             std::memory_order_acq_rel)) {
        m_wakeFault.store(true, std::memory_order_release);
        Disable();
    }
#endif
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
    for (std::uint32_t id = 0; id < m_threadCount; ++id) {
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
        if (id == withheldWorker) {
            continue;
        }
#endif
        const kern_return_t result = semaphore_signal(
            static_cast<semaphore_t>(m_wakeSemaphores[id]));
        if (result != KERN_SUCCESS) {
            m_wakeFault.store(true, std::memory_order_release);
            Disable();
        }
    }
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
    for (std::uint32_t id = 0; id < m_threadCount; ++id) {
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
        if (id == withheldWorker) {
            continue;
        }
#endif
        m_wakeSemaphores[id]->release();
    }
#else
    m_epoch.notify_all();
#endif
}

bool RenderWorkerPool::WaitForEpochChange(std::uint32_t id,
                                          std::uint64_t seen) {
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE || HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
    (void)seen;
#endif
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
    EnterTestGate(render_worker_pool_test::TestGatePoint::BeforeWait);
#endif
    const kern_return_t result = semaphore_wait(
        static_cast<semaphore_t>(m_wakeSemaphores[id]));
    if (result != KERN_SUCCESS && result != KERN_ABORTED) {
        m_wakeFault.store(true, std::memory_order_release);
        Disable();
        return false;
    }
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
    EnterTestGate(render_worker_pool_test::TestGatePoint::BeforeWait);
#endif
    m_wakeSemaphores[id]->acquire();
#else
    (void)id;
    m_epoch.wait(seen, std::memory_order_acquire);
#endif
    return true;
}

void RenderWorkerPool::DestroyWakePrimitives() {
#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
    if (!m_wakePrimitivesDestroyed) {
        for (std::uint32_t semaphore : m_wakeSemaphores) {
            semaphore_destroy(mach_task_self(),
                              static_cast<semaphore_t>(semaphore));
        }
        m_wakePrimitivesDestroyed = true;
    }
    // During fault teardown workers may still be about to read their handle.
    // Keep the array stable until join completes; a wait on a destroyed port
    // returns an error. The post-join call clears it without destroying twice.
    if (m_threads.empty()) {
        m_wakeSemaphores.clear();
    }
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
    if (!m_wakeSemaphores.empty()) {
        m_wakeSemaphores.clear();
    }
#endif
}

void RenderWorkerPool::RecordJobFault() {
    m_jobFaultCount.fetch_add(1, std::memory_order_relaxed);
    m_jobFault.store(true, std::memory_order_release);
    Disable();
}

void RenderWorkerPool::WorkerMain(std::uint32_t id) {
    // Each worker tracks the last epoch it serviced; a new batch (or stop) is
    // an epoch it has not seen. The selected wait retains or rechecks a wake
    // from `seen`, so a wake published between the load and the wait is not
    // lost — wait() returns immediately when the value already moved.
    //
    // This address-wait is the ONLY blocking wait in the pool, and it happens
    // exclusively on a worker thread while it is parked between batches — off
    // the hot path. The audio callback thread (the coordinator in Run()) never
    // address-waits: it publishes a batch with a non-blocking notify and joins
    // by yield-spinning on an atomic counter, so no OS wait ever blocks the
    // callback.
    std::uint64_t seen = 0;
    RealtimeWorkerDomain joinedDomain{};
    bool joined = false;
    bool readyPublished = false;
    try {
        floatmode::ScopedFlushDenormals floatModeScope;

        // Publish readiness before the test-only pre-load gate. Start must be
        // able to return with a worker deliberately held in the narrow window
        // before its first epoch load; the subsequent epoch/wake protocol is
        // what proves that window cannot lose a command.
        m_threadsReady.fetch_add(1, std::memory_order_release);
        readyPublished = true;
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
        EnterTestGate(
            render_worker_pool_test::TestGatePoint::BeforeFirstEpochLoad);
#endif
        for (;;) {
            std::uint64_t epoch = m_epoch.load(std::memory_order_acquire);
            while (epoch == seen) {
                if (!WaitForEpochChange(id, seen)) {
                    break;
                }
                epoch = m_epoch.load(std::memory_order_acquire);
            }
            if (m_wakeFault.load(std::memory_order_acquire)) {
                break;
            }
            seen = epoch;
            if (m_stop.load(std::memory_order_acquire)) {
                break;
            }
            const BatchDescriptor& batch = m_batches[SlotOf(epoch)];
            switch (batch.command) {
            case Command::RunBatch:
                RunBatch(id, epoch);
                break;
            case Command::BindDomain: {
                const RealtimeWorkerDomain domain = m_controlDomain;
                RealtimeWorkerJoinResult joinResult =
                    RealtimeWorkerJoinResult::FailedClean;
                if (!joined && domain.generation != 0 &&
                    domain.ops.join != nullptr &&
                    domain.ops.leave != nullptr) {
                    joinResult = domain.ops.join(
                        domain.context, id, domain.generation);
                } else if (joined) {
                    // Retain the local descriptor so rollback can remove the
                    // pre-existing membership before this worker exits.
                    joinResult =
                        RealtimeWorkerJoinResult::FailedNeedsLeave;
                }
                if (joinResult == RealtimeWorkerJoinResult::Joined ||
                    joinResult ==
                        RealtimeWorkerJoinResult::FailedNeedsLeave) {
                    if (!joined) {
                        joinedDomain = domain;
                    }
                    joined = true;
                    m_workerDomainGenerations[id] =
                        joinedDomain.generation;
                } else {
                    m_workerDomainGenerations[id] = 0;
                }
                if (joinResult != RealtimeWorkerJoinResult::Joined) {
                    m_controlFailures.fetch_add(
                        1, std::memory_order_relaxed);
                }
                m_workerControlAckGenerations[id] = domain.generation;
                m_controlFinished.fetch_add(1, std::memory_order_acq_rel);
                break;
            }
            case Command::LeaveDomain: {
                const RealtimeWorkerDomain commandDomain = m_controlDomain;
                if (joined) {
                    bool generationMatches = true;
                    if (joinedDomain.generation !=
                        commandDomain.generation) {
                        generationMatches = false;
                    }
                    const bool left =
                        LeaveJoinedDomain(joinedDomain, id);
                    if (left) {
                        joinedDomain = RealtimeWorkerDomain{};
                        joined = false;
                        m_workerDomainGenerations[id] = 0;
                    }
                    if (!left || !generationMatches) {
                        m_controlFailures.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                } else {
                    m_workerDomainGenerations[id] = 0;
                }
                m_workerControlAckGenerations[id] =
                    commandDomain.generation;
                m_controlFinished.fetch_add(1, std::memory_order_acq_rel);
                break;
            }
            case Command::None:
                break;
            }
        }
    } catch (...) {
        if (!readyPublished) {
            m_threadStartupFaults.fetch_add(1, std::memory_order_relaxed);
            m_threadsReady.fetch_add(1, std::memory_order_release);
            readyPublished = true;
        } else {
            // Worker entry is an engine-owned thread boundary. Do not let an
            // unexpected exception terminate the process; account for the
            // active command so its owner can leave the barrier.
            RecordJobFault();
            const std::uint64_t faultEpoch =
                m_epoch.load(std::memory_order_acquire);
            const Command faultCommand = m_batches[SlotOf(faultEpoch)].command;
            if (faultCommand == Command::RunBatch) {
                m_workersFinished[SlotOf(faultEpoch)].fetch_add(
                    1, std::memory_order_acq_rel);
            } else if (faultCommand == Command::BindDomain ||
                       faultCommand == Command::LeaveDomain) {
                m_controlFailures.fetch_add(1,
                                            std::memory_order_relaxed);
                m_workerControlAckGenerations[id] =
                    m_controlDomain.generation;
                m_controlFinished.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }

    // Domain teardown is part of worker-thread exit, not pool-owner cleanup.
    // This covers Stop and the forced wake-primitive teardown path while the
    // owner still retains the descriptor context through join().
    if (joined) {
        (void)LeaveJoinedDomain(joinedDomain, id);
        m_workerDomainGenerations[id] = 0;
    }
}

void RenderWorkerPool::RunBatch(std::uint32_t id, std::uint64_t generation) {
    // Tagged for the batch: the allocation guard is armed on this thread while
    // it runs cascades, so any heap touch inside a job aborts (base spec §4).
    memguard::AudioThreadScope scope;
    const std::uint64_t start = CpuClockTicks();
    const std::uint32_t slot = SlotOf(generation);
    const BatchDescriptor& batch = m_batches[slot];
    const std::uint32_t jobCount = batch.jobCount;
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    rtprobe::Sink* probeSink = m_probeBatchSink;
    rtprobe::WorkerScratch* probeScratch =
        probeSink != nullptr && id < probeSink->workerScratchCapacity
            ? &probeSink->workerScratch[id]
            : nullptr;
    std::uint32_t probeJobsCompleted = 0;
    if (probeScratch != nullptr) {
        probeScratch->writerToken = rtprobe::CurrentThreadToken();
        probeScratch->observedEpoch =
            m_epoch.load(std::memory_order_acquire);
        probeScratch->runBeginTicks = start;
        probeScratch->doneTicks = 0;
        probeScratch->jobsCompleted = 0;
        probeScratch->flags = rtprobe::kWorkerScratchFlagNone;
        std::uint64_t& expectedToken = probeSink->workerTokens[id];
        if (expectedToken == 0) {
            expectedToken = probeScratch->writerToken;
        } else if (expectedToken != probeScratch->writerToken) {
            probeScratch->flags |=
                rtprobe::kWorkerScratchFlagIdentityMismatch;
        }
    }
#endif
    for (;;) {
        // Stop claiming the moment this batch is no longer the live one.
        //
        // Defensive, with no known reachable path today: the coordinator drains
        // the shared claim cursor before it can reach any wait, so a batch it
        // walks away from has nothing left to claim and the size test below
        // stops a resumed worker on its own. What actually bounds the quarantine
        // set to one job per executor is the shape of this loop -- claim, run,
        // account, repeat -- which never holds two jobs at once.
        //
        // It becomes load-bearing the moment the coordinator can stop claiming
        // early, because then a dead batch does have unclaimed jobs and every
        // one a stranded worker takes is another voice slot held for good.
        if (m_epoch.load(std::memory_order_acquire) != generation) {
            break;
        }
        // Dynamic claim: workers and the coordinator share one monotonic
        // cursor, so the load balances regardless of per-voice cost. The
        // reduction order is fixed by the serial fold, not by which thread
        // ran which job, so any distribution is deterministic (§8.2).
        const std::uint32_t j =
            m_nextJob[slot].fetch_add(1, std::memory_order_relaxed);
        if (j >= jobCount) {
            break;
        }
        // Names this job as claimed by this generation, so the coordinator can
        // tell a finished job from one still in a stranded worker's hands.
        m_jobState[j].store(EncodeJobState(generation, JobState::Claimed),
                            std::memory_order_release);
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
        // Held here, a worker is in exactly the state a quarantine exists for:
        // this job is registered as claimed by this generation and has not run.
        // The coordinator's view of it is identical to a real stall, because the
        // only inputs it has are the progress word and the finished counter.
        EnterTestGate(
            render_worker_pool_test::TestGatePoint::BeforeJobFunction);
#endif
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
        // This worker already published the actual generation's native Claim.
        EnterOriginalSourceWorkerClaimGate(*this, generation, j);
#endif
        try {
            batch.fn(batch.userData, j);
        } catch (...) {
            // A failed invocation still consumes its claimed slot. Recording
            // that completion lets the coordinator drain this batch without
            // waiting forever for work that can no longer report normally.
            RecordJobFault();
        }
        m_jobState[j].store(EncodeJobState(generation, JobState::Done),
                            std::memory_order_release);
        m_jobsCompleted[slot].fetch_add(1, std::memory_order_release);
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        ++probeJobsCompleted;
#endif
    }
    // Plain store; the finished increment below publishes it. acq_rel chains
    // the increments in modification order — each RMW acquires the prior — so
    // the coordinator's acquire load of the final count happens-after every
    // worker's cascade writes (the produced scratch) and busy store, which is
    // what makes the serial fold read fully-computed scratch.
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    const std::uint64_t done = CpuClockTicks();
    m_workerBusyTicks[slot][id] = done - start;
    if (probeScratch != nullptr) {
        probeScratch->doneTicks = done;
        probeScratch->jobsCompleted = probeJobsCompleted;
    }
#else
    m_workerBusyTicks[slot][id] = CpuClockTicks() - start;
#endif
    m_workersFinished[slot].fetch_add(1, std::memory_order_acq_rel);
}

bool RenderWorkerPool::JobCompletedForLastBatch(std::uint32_t j) const {
    if (m_abandonedGeneration == 0 || j >= m_abandonedJobCount) {
        return true;
    }
    return m_jobDoneAtAbandonment[j] != 0;
}

template <typename Ready>
std::uint64_t RenderWorkerPool::SpinUntil(Ready ready,
                                          std::uint64_t deadlineAt,
                                          std::uint64_t* yieldCounter) {
    while (!ready()) {
        if (deadlineAt != 0) {
            const std::uint64_t now = CpuClockTicks();
            if (now > deadlineAt) {
                return now;
            }
        }
        if (yieldCounter != nullptr) {
            ++*yieldCounter;
        }
        std::this_thread::yield();
    }
    return 0;
}

RenderWorkerPool::RunResult RenderWorkerPool::AbandonBatch(
    std::uint64_t generation, std::uint32_t jobCount,
    std::uint64_t deadlineAt, std::uint64_t observedTicks) {
    // Void the generation. The command of the slot the NEXT generation selects
    // has to be cleared BEFORE the epoch moves, and the order is not cosmetic: a
    // worker that wakes reloads the epoch and then switches on that slot's
    // command unconditionally. Bumping the epoch alone would point it at a
    // descriptor two generations old, whose command is very likely still
    // RunBatch and whose function and job count are stale -- it would go run a
    // dead batch, and the epoch would agree with it. Cleared first, it falls
    // through the command switch and parks again.
    m_batches[SlotOf(generation + 1)].command = Command::None;
    m_epoch.fetch_add(1, std::memory_order_release);
    // No wake here. A stranded worker is not parked -- it is inside the batch
    // and rereads the epoch on its own -- so signalling would only wake the
    // workers that are parked, to look at a cleared command and go back to
    // sleep, at the cost of a notify per worker on the audio callback thread.

    // Classify once, now that the generation is void. The set can only shrink
    // from here: the dead generation cannot claim anything more, so a word can
    // still move from claimed to done but never the other way, which is what
    // makes a single reading safe to keep.
    const std::uint64_t doneWord = EncodeJobState(generation, JobState::Done);
    std::uint32_t unfinished = 0;
    for (std::uint32_t j = 0; j < jobCount; ++j) {
        const bool complete =
            m_jobState[j].load(std::memory_order_acquire) == doneWord;
        m_jobDoneAtAbandonment[j] = complete ? std::uint8_t{1}
                                             : std::uint8_t{0};
        if (!complete) {
            ++unfinished;
        }
    }
    m_abandonedGeneration = generation;
    m_abandonedJobCount = jobCount;

    // Later blocks render serially on the caller. This is what makes two copies
    // of the batch state sufficient: nothing publishes again, so the copy the
    // stranded worker is reading is never rewritten.
    m_serialFallback = true;
    Disable();

    RunResult result{};
    result.abandoned = true;
    result.unfinishedJobs = unfinished;
    result.overrunTicks =
        observedTicks > deadlineAt ? observedTicks - deadlineAt : 0;
    FinishReservedBatch();
    return result;
}

RenderWorkerPool::RunResult RenderWorkerPool::Run(std::uint32_t jobCount,
                                                  JobFn fn, void* userData,
                                                  std::uint64_t deadlineTicks) {
    assert(jobCount <= m_configuredMaxJobs);
    if (m_deadlineTicksOverride != 0) {
        deadlineTicks = m_deadlineTicksOverride;
    }
    // The generation this call will publish as, and therefore which of the two
    // copies of the batch state it owns. Fixed here because the probe helper
    // below closes over it; only the publication further down makes it live.
    const std::uint64_t generation =
        m_epoch.load(std::memory_order_relaxed) + 1;
    const std::uint32_t slot = SlotOf(generation);
    bool hasReservation =
        m_runActive.load(std::memory_order_acquire);
    if (!hasReservation && m_threadCount != 0) {
        hasReservation = TryReserveBatch();
    }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    rtprobe::Sink* probeSink = rtprobe::WorkerBarrierSink();
    rtprobe::WorkerBarrierRecord probeRecord{};
    if (probeSink != nullptr) {
        probeRecord.coordinatorToken = rtprobe::CurrentThreadToken();
        probeRecord.jobCount = jobCount;
        probeRecord.workerCount = m_threadCount;
        if (probeSink->workerCoordinatorToken == 0) {
            probeSink->workerCoordinatorToken =
                probeRecord.coordinatorToken;
        } else if (probeSink->workerCoordinatorToken !=
                   probeRecord.coordinatorToken) {
            probeRecord.flags |=
                rtprobe::kWorkerBarrierFlagCoordinatorIdentityMismatch;
            ++probeSink->workerCoordinatorMismatchCount;
        }
        if (probeSink->deviceGeneration != 0 &&
            probeSink->coordinatorToken != 0 &&
            probeSink->coordinatorToken != probeRecord.coordinatorToken) {
            probeRecord.flags |=
                rtprobe::kWorkerBarrierFlagCoordinatorIdentityMismatch;
            ++probeSink->workerCoordinatorMismatchCount;
        }
    }
    const auto finalizeProbe =
        [&](std::uint32_t extraFlags, bool readScratch) noexcept {
            if (probeSink == nullptr) {
                return;
            }
            probeRecord.barrierDoneTicks = CpuClockTicks();
            if (probeRecord.jobsCompleted == 0) {
                probeRecord.jobsCompleted =
                    m_jobsCompleted[slot].load(std::memory_order_acquire);
            }
            probeRecord.workersFinished =
                m_workersFinished[slot].load(std::memory_order_acquire);
            probeRecord.flags |= extraFlags;
            if (m_wakeFault.load(std::memory_order_acquire)) {
                probeRecord.flags |=
                    rtprobe::kWorkerBarrierFlagWakeFault;
            }
            if (m_jobFault.load(std::memory_order_acquire)) {
                probeRecord.flags |=
                    rtprobe::kWorkerBarrierFlagJobFault;
            }
            if (m_serialFallback) {
                probeRecord.flags |=
                    rtprobe::kWorkerBarrierFlagSerialFallback;
            }
            if (readScratch) {
                bool haveWorkerRun = false;
                for (std::uint32_t i = 0; i < m_threadCount; ++i) {
                    if (i >= probeSink->workerScratchCapacity) {
                        probeRecord.flags |=
                            rtprobe::kWorkerBarrierFlagIncompleteScratch;
                        continue;
                    }
                    const rtprobe::WorkerScratch& scratch =
                        probeSink->workerScratch[i];
                    if (scratch.doneTicks == 0 ||
                        scratch.runBeginTicks == 0 ||
                        scratch.observedEpoch != probeRecord.epoch) {
                        probeRecord.flags |=
                            rtprobe::kWorkerBarrierFlagIncompleteScratch;
                    }
                    if ((scratch.flags &
                         rtprobe::kWorkerScratchFlagIdentityMismatch) != 0) {
                        if (i < 64) {
                            probeRecord.identityMismatchMask |=
                                std::uint64_t{1} << i;
                        } else {
                            probeRecord.flags |=
                                rtprobe::kWorkerBarrierFlagIncompleteScratch;
                        }
                    }
                    if (scratch.runBeginTicks != 0) {
                        if (!haveWorkerRun ||
                            scratch.runBeginTicks <
                                probeRecord.firstWorkerRunTicks) {
                            probeRecord.firstWorkerRunTicks =
                                scratch.runBeginTicks;
                        }
                        if (!haveWorkerRun ||
                            scratch.runBeginTicks >
                                probeRecord.lastWorkerRunTicks) {
                            probeRecord.lastWorkerRunTicks =
                                scratch.runBeginTicks;
                        }
                        haveWorkerRun = true;
                    }
                    if (scratch.doneTicks >
                        probeRecord.lastWorkerDoneTicks) {
                        probeRecord.lastWorkerDoneTicks =
                            scratch.doneTicks;
                    }
                }
            }
            rtprobe::AppendWorkerBarrier(probeRecord);
        };
#endif
    const bool threadSetFault =
        m_serialFallback ||
        m_wakeFault.load(std::memory_order_acquire) ||
        m_jobFault.load(std::memory_order_acquire);
    if (m_threadCount == 0 || !hasReservation || threadSetFault) {
        // No pool or faulted pool: render serially on the caller. Run() is not
        // normally called without workers, but staying correct keeps the
        // contract total. Faults select this path until lifecycle recovery; it
        // never aborts and never claims that a worker was notified.
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        if (probeSink != nullptr) {
            probeRecord.epoch =
                m_epoch.load(std::memory_order_acquire);
            probeRecord.publishTicks = CpuClockTicks();
        }
#endif
        for (std::uint32_t j = 0; j < jobCount; ++j) {
            try {
                fn(userData, j);
            } catch (...) {
                // Serial degradation is still an audio callback boundary:
                // contain a later throwing job instead of leaking it out of
                // Run().
                RecordJobFault();
            }
        }
        if (threadSetFault ||
            m_jobFault.load(std::memory_order_acquire)) {
            m_serialFallback = true;
            Disable();
        }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        if (probeSink != nullptr) {
            probeRecord.coordinatorDoneTicks = CpuClockTicks();
            probeRecord.jobsCompleted = jobCount;
        }
        finalizeProbe(rtprobe::kWorkerBarrierFlagSerialFallback |
                          rtprobe::kWorkerBarrierFlagIncompleteScratch,
                      false);
#endif
        if (hasReservation) {
            FinishReservedBatch();
        }
        return RunResult{};
    }

    // The deadline starts before the batch exists, so the budget covers
    // publication and the coordinator's own share of the work as well as every
    // wait. A budget measured from the first wait would be blind to exactly the
    // case where the coordinator itself is what ran long.
    const std::uint64_t deadlineAt =
        deadlineTicks != 0 ? CpuClockTicks() + deadlineTicks : 0;
    const auto abandonIfExpired = [&]() -> RunResult {
        if (deadlineAt == 0) {
            return RunResult{};
        }
        const std::uint64_t now = CpuClockTicks();
        if (now <= deadlineAt) {
            return RunResult{};
        }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        if (probeSink != nullptr) {
            probeRecord.coordinatorDoneTicks = now;
        }
        finalizeProbe(rtprobe::kWorkerBarrierFlagIncompleteScratch, false);
#endif
        return AbandonBatch(generation, jobCount, deadlineAt, now);
    };

    // Publish the batch, then release the workers. The epoch bump is a release
    // store, so a worker that observes the new epoch also observes these
    // writes; m_nextJob and m_workersFinished are reset before the bump.
    // Writing the copy the new generation owns cannot disturb a worker still
    // reading the previous one.
    m_batches[slot].fn = fn;
    m_batches[slot].userData = userData;
    m_batches[slot].jobCount = jobCount;
    m_batches[slot].command = Command::RunBatch;
    m_nextJob[slot].store(0, std::memory_order_relaxed);
    m_workersFinished[slot].store(0, std::memory_order_relaxed);
    m_jobsCompleted[slot].store(0, std::memory_order_relaxed);
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    m_probeBatchSink = probeSink;
    if (probeSink != nullptr) {
        const std::uint32_t scratchCount =
            m_threadCount < probeSink->workerScratchCapacity
                ? m_threadCount
                : probeSink->workerScratchCapacity;
        for (std::uint32_t i = 0; i < scratchCount; ++i) {
            probeSink->workerScratch[i] = rtprobe::WorkerScratch{};
        }
        if (scratchCount != m_threadCount) {
            probeRecord.flags |=
                rtprobe::kWorkerBarrierFlagIncompleteScratch;
        }
        probeRecord.publishTicks = CpuClockTicks();
        probeRecord.epoch =
            m_epoch.fetch_add(1, std::memory_order_release) + 1;
    } else {
        m_epoch.fetch_add(1, std::memory_order_release);
    }
#else
    m_epoch.fetch_add(1, std::memory_order_release);
#endif
    NotifyEpochChanged();
    const bool wakeFault =
        m_wakeFault.load(std::memory_order_acquire) ||
        m_jobFault.load(std::memory_order_acquire);

    // The coordinator (the audio callback thread) participates so it does real
    // work instead of idling while the workers run. It is already tagged by the
    // mixer's per-block AudioThreadScope, so no scope is entered here.
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    // The real batch is published. This optional gate delays this executor so
    // a worker can claim the original Source job before its genuine deadline.
    EnterOriginalSourceCoordinatorGate(*this, generation);
#endif
    for (;;) {
        const RunResult deadlineResult = abandonIfExpired();
        if (deadlineResult.abandoned) {
            return deadlineResult;
        }
        const std::uint32_t j =
            m_nextJob[slot].fetch_add(1, std::memory_order_relaxed);
        if (j >= jobCount) {
            break;
        }
        m_jobState[j].store(EncodeJobState(generation, JobState::Claimed),
                            std::memory_order_release);
        try {
            fn(userData, j);
        } catch (...) {
            // The coordinator is itself an audio callback thread. A job
            // failure is counted as consumed so the batch can be drained.
            RecordJobFault();
        }
        m_jobState[j].store(EncodeJobState(generation, JobState::Done),
                            std::memory_order_release);
        m_jobsCompleted[slot].fetch_add(1, std::memory_order_release);
        const RunResult jobDeadlineResult = abandonIfExpired();
        if (jobDeadlineResult.abandoned) {
            return jobDeadlineResult;
        }
    }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    if (probeSink != nullptr) {
        probeRecord.coordinatorDoneTicks = CpuClockTicks();
    }
#endif

    // Every wait below is a rendezvous with the worker threads, and every one of
    // them can be held open by a worker that stopped making progress. They are
    // therefore all bounded by the same deadline and all end in the same
    // abandonment, including the two that are only reachable after a fault: a
    // wait on the audio callback thread does not become acceptable because the
    // reason for it was already a degradation.
    const auto jobsDrained = [&] {
        return m_jobsCompleted[slot].load(std::memory_order_acquire) ==
               jobCount;
    };
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    std::uint64_t* const drainYields =
        probeSink != nullptr ? &probeRecord.yieldIterations : nullptr;
#else
    std::uint64_t* const drainYields = nullptr;
#endif

    if (wakeFault) {
        // Workers that did receive a token may still be finishing claimed
        // jobs. Drain only the completed-job counter, so an unparked worker
        // cannot strand the callback at the worker barrier. The coordinator
        // executes every still-unclaimed job itself. Subsequent batches stay
        // serial until lifecycle recovery replaces this thread set.
        m_serialFallback = true;
        Disable();
        const std::uint64_t expiredAt =
            SpinUntil(jobsDrained, deadlineAt, drainYields);
        if (expiredAt != 0) {
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
            finalizeProbe(rtprobe::kWorkerBarrierFlagIncompleteScratch, false);
#endif
            return AbandonBatch(generation, jobCount, deadlineAt, expiredAt);
        }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        finalizeProbe(rtprobe::kWorkerBarrierFlagIncompleteScratch, false);
#endif
        FinishReservedBatch();
        return RunResult{};
    }

    // Join: spin-yield on the completion barrier (an atomic wait, never a
    // blocking mutex — the audio callback must not block on a lock). Once every
    // worker has drained the queue, every claimed job has completed. yield()
    // hands the core to a worker descheduled onto it rather than busy-burning.
    for (;;) {
        if (m_workersFinished[slot].load(std::memory_order_acquire) ==
            m_threadCount) {
            break;
        }
        if (m_wakeFault.load(std::memory_order_acquire) ||
            m_jobFault.load(std::memory_order_acquire)) {
            m_serialFallback = true;
            Disable();
            const std::uint64_t faultExpiredAt =
                SpinUntil(jobsDrained, deadlineAt, drainYields);
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
            finalizeProbe(rtprobe::kWorkerBarrierFlagIncompleteScratch,
                          false);
#endif
            if (faultExpiredAt != 0) {
                return AbandonBatch(generation, jobCount, deadlineAt,
                                    faultExpiredAt);
            }
            FinishReservedBatch();
            return RunResult{};
        }
        const RunResult waitDeadlineResult = abandonIfExpired();
        if (waitDeadlineResult.abandoned) {
            return waitDeadlineResult;
        }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        if (probeSink != nullptr) {
            ++probeRecord.yieldIterations;
        }
#endif
        std::this_thread::yield();
    }

    const RunResult finalDeadlineResult = abandonIfExpired();
    if (finalDeadlineResult.abandoned) {
        return finalDeadlineResult;
    }

#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    finalizeProbe(rtprobe::kWorkerBarrierFlagNone, true);
#endif

    m_batches[slot].command = Command::None;

    // Peak busy span across the worker threads for this batch — the
    // parallelism signal the snapshot reports. The coordinator's own span is
    // excluded: the figure answers "how long was the busiest worker busy."
    // Reading these words is only legal on this side of the barrier, which is
    // why an abandoned batch reports no peak at all.
    RunResult result{};
    for (std::uint32_t i = 0; i < m_threadCount; ++i) {
        if (m_workerBusyTicks[slot][i] > result.busyPeakTicks) {
            result.busyPeakTicks = m_workerBusyTicks[slot][i];
        }
    }
    FinishReservedBatch();
    return result;
}

} // namespace harken
