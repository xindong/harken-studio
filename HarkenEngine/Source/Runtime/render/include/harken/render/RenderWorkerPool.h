// Engine-owned render worker pool for job-parallel voice rendering. The pool's
// fixed-size thread set is created at engine init and may be recreated on the
// lifecycle thread after a complete fault teardown. Between blocks the threads
// park on an address-wait, so no thread is created and nothing is heap-allocated
// on the audio path (base spec §4; M5 §8.1).
//
// A block's independent per-voice cascades are dispatched here as jobs. The
// worker threads are tagged audio threads for the duration of a batch — the
// Debug allocation guard is armed on them, and a job that touches the system
// heap aborts, exactly as on the callback thread. The parallelism covers the
// cascade only; the bus fold-in (the order-dependent float reduction) runs
// single-threaded in the caller after the join, which is what keeps the mix
// deterministic and independent of the worker count (M5 §8.2).
//
// Coordination is lock-free on the hot path (M5 §8.1 RT constraint): the
// dispatching (audio callback) thread publishes the batch by bumping an epoch
// and waking parked workers, participates in the work itself so it is never
// idle, and then spin-yields on a completion counter until every worker has
// drained the queue. Idle workers normally block on a C++20 atomic wait. The
// iOS 13 fallback instead owns one counting Mach semaphore per worker, so an
// epoch signal made before that worker parks remains as a durable token. No
// blocking mutex is ever taken on the audio callback thread.
#ifndef HARKEN_RENDER_RENDER_WORKER_POOL_H
#define HARKEN_RENDER_RENDER_WORKER_POOL_H

#include "harken/render/RenderWorkerPoolWakeConfig.h"

#include <atomic>
#include <cstdint>
#include <memory>
#if HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
#include <semaphore>
#endif
#include <thread>
#include <type_traits>
#include <vector>

namespace harken {

class RenderWorkerPool;
namespace detail { struct VoicePoolReleaseTestAccess; }
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace original_source_worker_test {
using CoordinatorGate = void (*)(void*, std::uint64_t) noexcept;
using ClaimGate = void (*)(void*, std::uint64_t, std::uint32_t) noexcept;
// Test-linked native library only. Configure between completed callbacks and
// retain context until actual Stop/join. No RenderWorkerPool layout changes.
void Configure(RenderWorkerPool&, CoordinatorGate, ClaimGate, void*) noexcept;
}
#endif

// Join distinguishes a clean refusal from a platform call that failed its
// validation while retaining per-thread membership. The latter must still run
// Leave on that worker before the backend may release the domain context.
enum class RealtimeWorkerJoinResult : std::uint8_t {
    FailedClean,
    Joined,
    FailedNeedsLeave,
};

// Platform backends use this narrow interface to place every render worker in
// the callback's real-time scheduling domain. Both callbacks execute on the
// addressed worker thread. A successful Join must be paired with a successful
// Leave, or with termination of that worker, before the domain context may be
// destroyed. FailedNeedsLeave makes partial platform setup part of the same
// rollback transaction instead of hiding it behind a failed admission.
struct RealtimeWorkerDomainOps {
    using JoinFn = RealtimeWorkerJoinResult (*)(
        void* context, std::uint32_t workerId,
        std::uint64_t generation) noexcept;
    using LeaveFn = bool (*)(void* context, std::uint32_t workerId,
                             std::uint64_t generation) noexcept;

    JoinFn join = nullptr;
    LeaveFn leave = nullptr;
};

// Non-owning, trivially copyable scheduling-domain descriptor. Generation is
// chosen by the backend and must be non-zero. It binds acknowledgements to one
// device-open attempt, so a retry cannot consume stale completion state from a
// prior domain.
struct RealtimeWorkerDomain {
    RealtimeWorkerDomainOps ops{};
    void* context = nullptr;
    std::uint64_t generation = 0;
};
static_assert(std::is_trivially_copyable_v<RealtimeWorkerDomainOps> &&
                  std::is_standard_layout_v<RealtimeWorkerDomainOps> &&
                  std::is_trivially_copyable_v<RealtimeWorkerDomain> &&
                  std::is_standard_layout_v<RealtimeWorkerDomain>,
              "real-time worker domain descriptors must remain PODs");

#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
namespace rtprobe {
struct Sink;
}
#endif

#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
namespace render_worker_pool_test {
enum class TestGatePoint : std::uint32_t {
    BeforeFirstEpochLoad = 1,
    BeforeWait = 2,
    // Inside a batch, after the worker has registered the job it claimed and
    // before it runs it. This is the only point that holds a worker in the
    // state a quarantine exists for: claimed by this generation, not finished.
    BeforeJobFunction = 3,
};

// An ordered gate carries its own release/acquire edge, so a stall it models is
// more synchronized than a real one and can hide a missing release in the code
// under test. The unordered variant enters and observes its release with
// relaxed operations only, adding no edge of its own.
//
// This removes the gate's own bias; it does not make the resulting test a
// memory-order proof. On x86's TSO every plain store already carries release
// semantics, so a missing release very likely still passes here. The
// discriminating platforms are the weakly ordered ones.
enum class TestGateOrdering : std::uint32_t {
    Ordered = 0,
    Unordered = 1,
};

void ArmBeforeFirstEpochLoadGate(std::uint32_t expectedWorkers);
void ArmBeforeWaitGate(std::uint32_t expectedWorkers);
void ArmBeforeJobFunctionGate(std::uint32_t expectedWorkers,
                              TestGateOrdering ordering);
std::uint32_t GateEnteredCount(TestGatePoint point);
void ReleaseGate(TestGatePoint point);
std::uint64_t NotificationCount();
void NotifyWithoutEpochChange(RenderWorkerPool& pool);
void FailNextWakeNotification();
void WithholdNextWakeNotificationForWorker(std::uint32_t workerId);
// Zero-based worker index. The request is consumed by the next thread-set
// creation, including a lifecycle recovery and an out-of-range index.
void FailNextStartBeforeWorker(std::uint32_t workerId);
bool IsSerialFallback(const RenderWorkerPool& pool);
std::uint32_t JobFaultCount(const RenderWorkerPool& pool);
bool HasCleanRestartState(const RenderWorkerPool& pool);
}
#endif

class RenderWorkerPool {
public:
    // One unit of work: run the job at `jobIndex` (0-based, < the dispatched
    // count). The pool passes `userData` through unchanged. A job must not
    // touch the system heap, take a blocking lock, or do I/O — it runs on a
    // tagged audio thread.
    using JobFn = void (*)(void* userData, std::uint32_t jobIndex);

    RenderWorkerPool() = default;
    ~RenderWorkerPool();

    RenderWorkerPool(const RenderWorkerPool&) = delete;
    RenderWorkerPool& operator=(const RenderWorkerPool&) = delete;

    // Creates `workerCount` disabled worker threads and waits until every
    // thread has entered its command loop (logic thread, at init). A count of
    // 0 succeeds with an empty pool. Start reports every resource failure,
    // including a wake-primitive failure, and fully unwinds partial creation.
    // It may be called at most once per pool instance (debug-asserted). The
    // requested count is frozen for the engine lifetime; EnsureReady may later
    // recreate that same count after a complete teardown, but cannot resize it.
    //
    // maxJobs bounds any single Run's jobCount and sizes the per-job state
    // array, so the audio thread never grows it. It is frozen with the worker
    // count.
    [[nodiscard]] bool Start(std::uint32_t workerCount,
                             std::uint32_t maxJobs) noexcept;

    // Ensures that the init-time fixed worker set exists in a healthy disabled
    // state. A healthy set is reused. A stopped or faulted set is fully joined
    // and recreated at the same frozen count. Partial recreation failure fully
    // unwinds and a later call may retry. Lifecycle thread only, with no live
    // callback, active batch, or retained real-time-domain binding; this method
    // allocates and creates threads and must never run on an audio thread.
    [[nodiscard]] bool EnsureReady() noexcept;

    // Binds the fixed worker set to a backend-owned real-time scheduling
    // domain. Join runs on each worker. The call returns only after every
    // worker acknowledged the supplied generation. If any Join fails, a
    // second control round makes every successfully joined worker run Leave;
    // the failed bind therefore has no retained domain membership and may be
    // retried with another generation when rollback succeeds. A failed Leave
    // force-stops the current thread set; after the domain context is released,
    // EnsureReady may recreate the same fixed count for a later Resume. The
    // domain context must remain alive until this call returns.
    bool BindRealtimeDomain(const RealtimeWorkerDomain& domain);

    // Disables parallel dispatch and makes every joined worker run Leave.
    // Calling before the callback has stopped returns false without unbinding;
    // the owner must stop it and retry while retaining the context. A Leave or
    // worker-infrastructure failure instead force-stops and joins the current
    // worker set before returning. EnsureReady may then recreate the init-time
    // fixed count. On the precondition refusal the context must remain alive
    // until a later successful Unbind or Stop.
    bool UnbindRealtimeDomain();

    // Enables workers after a successful real-time-domain bind. This is the
    // only mode suitable for a real device callback.
    bool EnableRealtime() noexcept;

    // Enables workers without a platform scheduling domain. This is reserved
    // for the Null/offline backend and deterministic harnesses; it must not be
    // used to claim real-device deadline safety.
    bool EnableOffline() noexcept;

    // Removes the worker set from eligibility for subsequent batches. This is
    // the only lifecycle operation allowed to overlap an in-flight Run: that
    // already-published batch completes normally, while later blocks observe
    // the atomic gate as closed. Disabling does not unbind the domain.
    void Disable() noexcept;

    // Signals the workers to exit and joins them (logic thread, at term).
    // A wake fault forces every fallback waiter out before joining, so Stop
    // does not depend on the failed notification. Stop must not overlap an
    // in-flight Run; the engine lifecycle guarantees that ordering and debug
    // builds assert it. Idempotent; the destructor calls it.
    void Stop();

    // Active batch participants. Fixed threads that are merely created or
    // domain-bound but disabled intentionally report zero.
    std::uint32_t WorkerCount() const noexcept {
        return m_dispatchState.load(std::memory_order_acquire) !=
                       DispatchState::Disabled
                   ? m_threadCount
                   : 0;
    }

    // Atomically reserves the current callback block's right to dispatch. It
    // closes the read-WorkerCount/Run gap during shutdown: Disable either wins
    // first and this returns false, or this wins and the selected batch remains
    // valid until Run (or CancelReservedBatch) releases the reservation.
    bool TryReserveBatch() noexcept;
    void CancelReservedBatch() noexcept;

    // What one Run produced. A completed batch reports busyPeakTicks and
    // nothing else; an abandoned one reports no busy peak (see below) and names
    // how much work the caller must treat as still in a stranded worker's
    // hands.
    struct RunResult {
        // Peak busy span, in performance-counter ticks, that any single worker
        // thread spent in this batch — the raw figure behind the snapshot's
        // renderWorkerBusyPeakUs (the caller converts to microseconds).
        //
        // Always 0 when abandoned == true. The per-worker busy words are
        // published by each worker's finished increment, and abandonment is
        // precisely the case where that increment never arrived, so reading
        // them would be a race, not a measurement.
        std::uint64_t busyPeakTicks = 0;
        // Ticks by which the rendezvous ran past its deadline before the
        // coordinator gave up. Not bounded by the poll interval: the
        // coordinator runs its own share of the jobs before it ever reaches a
        // wait, so a batch can already be far past the deadline at the first
        // check. 0 when abandoned == false.
        std::uint64_t overrunTicks = 0;
        // Jobs of this batch that were not observed finished at the moment the
        // coordinator classified them. Equal, by construction, to the number of
        // slots the caller must quarantine: the classification that produced
        // this count is the same one JobCompletedForLastBatch answers from.
        std::uint32_t unfinishedJobs = 0;
        // The coordinator left this batch with workers possibly still inside
        // it. Parallel dispatch is off for the life of this thread set, and the
        // caller owes the quarantine described in JobCompletedForLastBatch.
        bool abandoned = false;
    };

    // Runs `jobCount` jobs across the worker threads and the calling thread.
    // `fn(userData, j)` runs exactly once for each j in [0, jobCount) unless
    // the batch is abandoned. Audio thread; must only be called when
    // WorkerCount() > 0.
    //
    // deadlineTicks bounds the whole rendezvous — dispatch plus execution plus
    // every wait — measured from before the batch is published, so a budget
    // burned by the coordinator's own share still counts. Every wait a stranded
    // worker can hold the coordinator in is capped by it, not just the ordinary
    // completion barrier: a wait that is only reachable after a fault is still
    // a wait on the audio callback thread. 0 means unbounded, which is correct
    // only for callers with no deadline to keep (harnesses); the mixer always
    // passes the budget it computed.
    RunResult Run(std::uint32_t jobCount, JobFn fn, void* userData,
                  std::uint64_t deadlineTicks = 0);

    // Whether job `j` of the last abandoned batch was observed finished. Only
    // meaningful for j < that batch's jobCount, and only after Run reported
    // abandoned; every other case answers true, because a batch that reached
    // its barrier finished every job it claimed.
    //
    // The answer comes from the single classification the abandonment took, not
    // from a fresh read of the progress word. Re-reading would only ever
    // upgrade a job from unfinished to finished — the abandoned generation can
    // claim nothing more, so its words move one way — and buying one fewer
    // quarantined slot with a second racing read would also make the reported
    // unfinished count stop matching the set the caller actually holds.
    // Deliberately conservative, one read.
    bool JobCompletedForLastBatch(std::uint32_t j) const;

    // Internal deterministic failure-injection seam: when nonzero this budget
    // replaces whatever the caller passes to Run, so a test can make the
    // deadline expire without depending on how long real work takes. One
    // authority at a time — the override is not combined with the caller's
    // value. Set between blocks; never on an audio thread.
    void SetJobDeadlineTicksForTest(std::uint64_t deadlineTicks) {
        m_deadlineTicksOverride = deadlineTicks;
    }

private:
    // Keep the class definition identical in every linked target.
    friend struct detail::VoicePoolReleaseTestAccess;
#if defined(HARKEN_RENDER_WORKER_POOL_TEST_HOOKS)
    friend void render_worker_pool_test::NotifyWithoutEpochChange(
        RenderWorkerPool& pool);
    friend bool render_worker_pool_test::IsSerialFallback(
        const RenderWorkerPool& pool);
    friend std::uint32_t render_worker_pool_test::JobFaultCount(
        const RenderWorkerPool& pool);
    friend bool render_worker_pool_test::HasCleanRestartState(
        const RenderWorkerPool& pool);
#endif

    bool CreateThreadSet(std::uint32_t workerCount) noexcept;
    bool HasReadyThreadSet() const noexcept;
    void NotifyEpochChanged();
    bool WaitForEpochChange(std::uint32_t id, std::uint64_t seen);
    void WorkerMain(std::uint32_t id);
    void RunBatch(std::uint32_t id, std::uint64_t generation);
    bool DispatchDomainControl(bool bind,
                               const RealtimeWorkerDomain& domain);
    bool LeaveJoinedDomain(const RealtimeWorkerDomain& domain,
                           std::uint32_t id) noexcept;
    void DestroyWakePrimitives();
    void RecordJobFault();
    void FinishReservedBatch() noexcept;

    // Spin-yields until `ready()` holds or the absolute deadline passes.
    // Returns 0 when the wait completed, otherwise the tick reading that
    // observed the deadline gone. deadlineAt == 0 waits without a bound.
    //
    // One clock read per yield iteration. yield() itself costs an order of
    // magnitude more than a counter read, so polling every iteration does not
    // change what the spin costs; amortizing it would only blunt the reading.
    // yieldCounter, when given, is incremented once per yield so a probe build
    // still reports the same spin depth it did before these waits shared a
    // helper.
    template <typename Ready>
    std::uint64_t SpinUntil(Ready ready, std::uint64_t deadlineAt,
                            std::uint64_t* yieldCounter = nullptr);

    // Walks away from `generation`: voids it, classifies its jobs once, and
    // selects serial rendering for the rest of this thread set's life. Every
    // step is a plain store or an atomic RMW — no allocation, no lock, no
    // system call — so this cannot fail partway and leave the batch half
    // abandoned.
    RunResult AbandonBatch(std::uint64_t generation, std::uint32_t jobCount,
                           std::uint64_t deadlineAt,
                           std::uint64_t observedTicks);

    enum class Command : std::uint8_t {
        None,
        RunBatch,
        BindDomain,
        LeaveDomain,
    };

    enum class DispatchState : std::uint8_t {
        Disabled,
        Enabled,
        BatchActive,
        BatchActiveDisablePending,
    };
    static_assert(std::atomic<DispatchState>::is_always_lock_free,
                  "render-worker dispatch gate must be lock-free");

    std::vector<std::thread> m_threads;
    // The current physical thread-set count. Batch barriers always use this
    // count; WorkerCount exposes it only while dispatch is active.
    std::uint32_t m_threadCount = 0;
    // The init-time Start request survives forced pool teardown and is the only
    // count CreateThreadSet may recreate. A configured zero legitimately makes
    // lifecycle admission a no-op; no runtime path may resize this value.
    std::uint32_t m_configuredThreadCount = 0;
    bool m_startedOnce = false;
    std::atomic<std::uint32_t> m_threadsReady{0};
    std::atomic<std::uint32_t> m_threadStartupFaults{0};
    std::atomic<DispatchState> m_dispatchState{DispatchState::Disabled};
    bool m_domainBound = false;
    RealtimeWorkerDomain m_boundDomain{};

    // Batch release/park handshake. The coordinator bumps m_epoch (release)
    // after publishing the batch fields, then signals every worker. A fallback
    // worker reloads m_epoch with acquire after consuming its own persistent
    // semaphore token; the semaphore carries no application data. Stop uses
    // the identical release-bump-then-signal sequence, then forces all waiters
    // out if a notification fault was observed. Surplus tokens are harmless
    // because the epoch is authoritative. Counting-wake teardown can add one
    // final token per worker; normal consumption and the pool lifetime keep
    // the count below its theoretical implementation ceiling.
    std::atomic<std::uint64_t> m_epoch{0};
    std::atomic<bool> m_stop{false};
    // An unexpected wake-primitive return selects serial fallback for the
    // current set and activates bounded teardown. Lifecycle recovery may
    // replace the entire set after the callback is stopped.
    std::atomic<bool> m_wakeFault{false};
    // A job exception selects serial rendering for later batches on the current
    // set. This is separate from m_wakeFault because it must not trigger wake
    // primitive destruction while workers are still parked safely.
    std::atomic<bool> m_jobFault{false};
    std::atomic<std::uint32_t> m_jobFaultCount{0};
    std::atomic<bool> m_runActive{false};
    bool m_serialFallback = false;

#if HARKEN_RENDER_WORKER_POOL_MACH_WAKE
    std::vector<std::uint32_t> m_wakeSemaphores;
    // Lifecycle-thread state. Destroy may precede join to wake blocked workers,
    // while the handle array remains stable until every worker has exited.
    bool m_wakePrimitivesDestroyed = false;
#elif HARKEN_RENDER_WORKER_POOL_COUNTING_WAKE
    using CountingSemaphore =
        std::counting_semaphore<0x7fffffff>;
    std::vector<std::unique_ptr<CountingSemaphore>> m_wakeSemaphores;
#endif

    // Batch descriptor, published by the coordinator before the epoch bump and
    // read by workers after it (plain fields ordered by the epoch's
    // release/acquire; the claim cursor is atomic because workers claim from it).
    struct BatchDescriptor {
        JobFn fn = nullptr;
        void* userData = nullptr;
        std::uint32_t jobCount = 0;
        Command command = Command::None;
    };

    // Two of everything a published batch owns, selected by the generation's
    // low bit. This is what makes a batch the coordinator has walked away from
    // harmless: a worker still inside it reads and increments its own
    // generation's copy, so nothing it does lands in the accounting of the
    // generation that replaced it.
    //
    // Two is exactly sufficient, not a guess. In normal operation generation
    // g+2 cannot publish until g+1's barrier completed, so the copy it reuses
    // (g's) has no reader left. After an abandonment parallel dispatch is
    // permanently disabled, so nothing publishes again and the stranded
    // worker's copy is never rewritten.
    //
    // What this does NOT do: it cannot stop a stranded worker from writing
    // through pointers it already holds. Memory safety comes from the caller's
    // quarantine — it retires the descending arena span the batch handed out
    // and holds every unfinished job's voice slot, its pooled effect instances,
    // its PCM reference and its stream slot for good, so nothing a stranded
    // worker can still write is ever handed to someone else. The generation
    // only keeps a worker from starting new work and keeps its bookkeeping out
    // of the live batch.
    static constexpr std::uint32_t kGenerationSlots = 2;
    static std::uint32_t SlotOf(std::uint64_t generation) {
        return static_cast<std::uint32_t>(generation &
                                          (kGenerationSlots - 1));
    }

    // Per-job progress, one word per job, carrying the generation that owns it.
    // The coordinator needs to know WHICH jobs finished -- a count cannot name
    // the slots a quarantine has to cover.
    //
    // The generation in the word means no per-block clearing is needed:
    // "unclaimed for my generation" is simply "this word's generation is not
    // mine". That reading is defensive today and has no known reachable path.
    // The coordinator drains the shared claim cursor before it can reach any
    // wait, so by the time a batch can be abandoned every job below its count
    // has already been claimed by this generation and no older generation's
    // word is left to misread. It becomes load-bearing the moment a batch can
    // be abandoned with jobs still unclaimed.
    enum class JobState : std::uint64_t {
        Unclaimed = 0,
        Claimed = 1,
        Done = 2,
    };
    static constexpr std::uint32_t kJobStateBits = 2;
    static std::uint64_t EncodeJobState(std::uint64_t generation,
                                       JobState state) {
        return (generation << kJobStateBits) |
               static_cast<std::uint64_t>(state);
    }

    BatchDescriptor m_batches[kGenerationSlots];
    std::atomic<std::uint32_t> m_nextJob[kGenerationSlots];
    std::vector<std::atomic<std::uint64_t>> m_jobState;
    // The abandonment's one classification pass, one byte per job. Coordinator-
    // owned and written only there, so the count it reported and the verdict a
    // later query gives cannot drift apart. Sized with m_jobState for each
    // thread-set creation; the audio thread never grows it.
    //
    // Not cleared per batch. m_abandonedGeneration bounds what it may answer
    // for, which is what keeps a previous abandonment's bytes from leaking into
    // a later batch without paying a clear on every ordinary block.
    std::vector<std::uint8_t> m_jobDoneAtAbandonment;
    // The generation this pool walked away from, 0 when it never has, and that
    // batch's job count. A published generation is never 0, so 0 is unambiguous.
    std::uint64_t m_abandonedGeneration = 0;
    std::uint32_t m_abandonedJobCount = 0;
    // Test-only budget override; 0 leaves the caller's deadline in force.
    std::uint64_t m_deadlineTicksOverride = 0;
    std::uint32_t m_configuredMaxJobs = 0;
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    rtprobe::Sink* m_probeBatchSink = nullptr;
#endif

    // Completion barrier: every worker increments this once it has drained the
    // queue for the current batch (a release store). The coordinator waits for
    // it to reach m_threadCount (acquire), which — through the fetch-add
    // release sequence — also publishes every worker's busy-tick write. Both
    // counters are per-generation for the reason given at kGenerationSlots.
    std::atomic<std::uint32_t> m_workersFinished[kGenerationSlots];
    std::atomic<std::uint32_t> m_jobsCompleted[kGenerationSlots];

    // Logic-thread control barrier. The descriptor and command are published
    // by m_epoch's release increment. A worker writes its generation slots
    // before the release increment of m_controlFinished; the logic thread's
    // acquire wait therefore sees one complete acknowledgement per worker.
    RealtimeWorkerDomain m_controlDomain{};
    std::atomic<std::uint32_t> m_controlFinished{0};
    std::atomic<std::uint32_t> m_controlFailures{0};
    std::vector<std::uint64_t> m_workerControlAckGenerations;
    std::vector<std::uint64_t> m_workerDomainGenerations;

    // Per-worker busy-span ticks for the current batch, written by each worker
    // before its finished increment and read by the coordinator after the
    // barrier. Sized once for each fixed-count thread-set creation, and
    // per-generation so a stranded worker's late write cannot restate the busy
    // peak of the batch that replaced it.
    std::vector<std::uint64_t> m_workerBusyTicks[kGenerationSlots];
};

} // namespace harken

#endif // HARKEN_RENDER_RENDER_WORKER_POOL_H
