// Checks over the qualification probe facility itself, and over the render
// worker instrumentation that writes into it.
//
// The worker cases dispatch real batches, so they depend on the pool's dispatch
// lifecycle: Start creates a thread set but leaves dispatch disabled, and Run
// against a disabled pool degrades to running every job on the calling thread
// and reports itself as a serial fallback. A case that means to measure the
// worker rendezvous therefore has to enable dispatch as well as start it,
// otherwise it asserts against a path where no worker ever entered a batch --
// the barrier count stays zero, no per-worker scratch is read, and the
// assertions either fail or hold vacuously.
#include "harken/core/RtProbe.h"
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
#include "harken/render/RenderWorkerPool.h"
#endif

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

struct ProbeStorage {
    harken::rtprobe::TimelineRecord timeline[8]{};
    harken::rtprobe::WorkerBarrierRecord barriers[8]{};
    harken::rtprobe::WorkerScratch scratch[2]{};
    std::uint64_t workerTokens[2]{};
    harken::rtprobe::Sink sink{};

    ProbeStorage() {
        sink.timeline = timeline;
        sink.timelineCapacity = 8;
        sink.workerBarriers = barriers;
        sink.workerBarrierCapacity = 8;
        sink.workerScratch = scratch;
        sink.workerTokens = workerTokens;
        sink.workerScratchCapacity = 2;
    }
};

#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
constexpr std::uint32_t kProbeWorkerCount = 2;
constexpr std::uint32_t kProbeJobCount = 32;
// Both arrays are indexed by worker id up to kProbeWorkerCount, so raising the
// worker count without widening the storage has to be a compile error.
static_assert(sizeof(ProbeStorage::scratch) /
                  sizeof(harken::rtprobe::WorkerScratch) ==
              kProbeWorkerCount,
              "probe storage needs one scratch row per worker");
static_assert(sizeof(ProbeStorage::workerTokens) / sizeof(std::uint64_t) ==
              kProbeWorkerCount,
              "probe storage needs one token slot per worker");

void CountJob(void* userData, std::uint32_t);
bool StartAndEnable(harken::RenderWorkerPool& pool);
#endif

void TestAttachmentAndOverflow() {
    ProbeStorage storage;
    harken::rtprobe::Sink unrelated{};
    Expect(!harken::rtprobe::Attach(nullptr), "null sink accepted");
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    ProbeStorage missingTimeline;
    missingTimeline.sink.timeline = nullptr;
    Expect(!harken::rtprobe::Attach(&missingTimeline.sink),
           "missing timeline accepted");
#endif
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    ProbeStorage missingWorkerStorage;
    missingWorkerStorage.sink.workerBarriers = nullptr;
    Expect(!harken::rtprobe::Attach(&missingWorkerStorage.sink),
           "missing worker storage accepted");
#endif
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    Expect(!harken::rtprobe::Attach(&storage.sink),
           "double attachment accepted");
    Expect(!harken::rtprobe::Detach(&unrelated),
           "unrelated sink detached");

    for (std::uint32_t i = 0; i < storage.sink.timelineCapacity; ++i) {
        auto* record = harken::rtprobe::ReserveTimeline(
            harken::rtprobe::TimelineKind::DeviceCallback);
        Expect(record != nullptr, "timeline capacity lost");
        if (record != nullptr) {
            // A record the tool cannot place in order, or whose kind does not
            // say how to read its packed fields, is not a captured record.
            Expect(record->sequence == i, "timeline sequence drifted");
            Expect(record->kind ==
                       static_cast<std::uint16_t>(
                           harken::rtprobe::TimelineKind::DeviceCallback),
                   "timeline record kind drifted");
            if (i == 0) {
                // Fill it the way a real writer fills a record it reserved, so
                // the storage-reuse check further down has something to prove
                // was cleared rather than something that was already zero.
                record->writerToken = 0xA5A5A5A5A5A5A5A5ull;
                record->beginTicks = 1;
                record->flags = harken::rtprobe::kTimelineFlagMeterArmed;
            }
        }
    }
    Expect(harken::rtprobe::ReserveTimeline(
               harken::rtprobe::TimelineKind::DeviceCallback) == nullptr,
           "timeline overflow accepted");
    Expect(storage.sink.timelineOverflow == 1,
           "timeline overflow not counted");
    // The overflow figure is how many records were lost, so it has to keep
    // counting. A counter that stops at one would report a single dropped
    // record for a run that dropped thousands.
    Expect(harken::rtprobe::ReserveTimeline(
               harken::rtprobe::TimelineKind::DeviceCallback) == nullptr,
           "timeline overflow accepted");
    Expect(storage.sink.timelineOverflow == 2,
           "timeline overflow saturated");
    Expect(storage.sink.timelineCount == storage.sink.timelineCapacity,
           "a refused reservation consumed a record slot");
    Expect(harken::rtprobe::EnterDeviceCallback(),
           "callback writer refused");
    Expect(!harken::rtprobe::Detach(&storage.sink),
           "active callback sink detached");
    harken::rtprobe::LeaveDeviceCallback();
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");

    // Detach hands the storage back to the tool, which then reads it. Anything
    // written after that point corrupts what is being read, so a released sink
    // must accept nothing -- not a record, and not a counter bump.
    const std::uint32_t releasedCount = storage.sink.timelineCount;
    const std::uint64_t releasedOverflow = storage.sink.timelineOverflow;
    Expect(harken::rtprobe::ReserveTimeline(
               harken::rtprobe::TimelineKind::DeviceCallback) == nullptr,
           "a released sink still reserved a record");
    harken::rtprobe::AppendWorkerBarrier({});
    Expect(storage.sink.timelineCount == releasedCount &&
               storage.sink.timelineOverflow == releasedOverflow &&
               storage.sink.workerBarrierCount == 0 &&
               storage.sink.workerBarrierOverflow == 0,
           "a released sink was still written");

    // The same storage must be reusable for a second measurement, and start
    // clean: a stale count would append the new run after the old records, and
    // a stale overflow would charge this run for the previous run's losses.
    Expect(harken::rtprobe::Attach(&storage.sink),
           "released storage refused a second attachment");
    Expect(storage.sink.timelineCount == 0 &&
               storage.sink.timelineOverflow == 0 &&
               storage.sink.workerBarrierCount == 0 &&
               storage.sink.workerBarrierOverflow == 0,
           "a second attachment kept the previous run's capture counters");
    auto* reused = harken::rtprobe::ReserveTimeline(
        harken::rtprobe::TimelineKind::DeviceCallback);
    Expect(reused != nullptr, "reattached storage refused a record");
    if (reused != nullptr) {
        Expect(reused->sequence == 0,
               "a reused record kept a stale sequence");
        Expect(reused->writerToken == 0 && reused->beginTicks == 0 &&
                   reused->flags == 0,
               "a reused record kept stale fields");
    }
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}

void TestCaptureMask() {
    ProbeStorage storage;
    storage.sink.captureMask = harken::rtprobe::kCaptureDeviceCallback;
    storage.sink.workerBarriers = nullptr;
    storage.sink.workerBarrierCapacity = 0;
    storage.sink.workerScratch = nullptr;
    storage.sink.workerTokens = nullptr;
    storage.sink.workerScratchCapacity = 0;
    Expect(harken::rtprobe::Attach(&storage.sink),
           "device-only sink refused");
    Expect(harken::rtprobe::ReserveTimeline(
               harken::rtprobe::TimelineKind::MixPublication) == nullptr,
           "disabled publication capture accepted");
    Expect(storage.sink.timelineCount == 0 &&
               storage.sink.timelineOverflow == 0,
           "disabled publication capture consumed storage");
    harken::rtprobe::AppendWorkerBarrier({});
    Expect(storage.sink.workerBarrierCount == 0 &&
               storage.sink.workerBarrierOverflow == 0,
           "disabled worker capture consumed storage");
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    Expect(harken::rtprobe::WorkerBarrierSink() == nullptr,
           "disabled worker capture exposed a sink");
    std::atomic<std::uint32_t> completed{0};
    harken::RenderWorkerPool pool;
    // Enabled, not merely started. A started-but-disabled pool runs the batch
    // on this thread, and then the check below would hold because no worker ran
    // rather than because the mask stopped one from writing.
    Expect(StartAndEnable(pool), "worker pool refused to dispatch");
    pool.Run(kProbeJobCount, &CountJob, &completed);
    pool.Stop();
    Expect(completed.load(std::memory_order_relaxed) == kProbeJobCount,
           "masked worker run lost jobs");
    Expect(storage.sink.workerBarrierCount == 0 &&
               storage.sink.workerCoordinatorToken == 0,
           "masked worker run mutated probe state");
#endif
    Expect(harken::rtprobe::ReserveTimeline(
               harken::rtprobe::TimelineKind::DeviceCallback) != nullptr,
           "enabled device capture was refused");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}

void TestWorkerBarrierCapacity() {
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    ProbeStorage storage;
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    for (std::uint32_t i = 0;
         i < storage.sink.workerBarrierCapacity; ++i) {
        harken::rtprobe::WorkerBarrierRecord record{};
        record.epoch = i + 1u;
        harken::rtprobe::AppendWorkerBarrier(record);
        Expect(storage.barriers[i].sequence == i,
               "worker barrier sequence drifted");
    }
    Expect(storage.sink.workerBarrierCount ==
               storage.sink.workerBarrierCapacity,
           "worker barrier capacity lost");
    harken::rtprobe::AppendWorkerBarrier({});
    Expect(storage.sink.workerBarrierOverflow == 1,
           "worker barrier overflow not counted");
    // Same reason as the timeline side: this figure is a loss count, so it has
    // to keep counting, and a refused append must not eat a slot.
    harken::rtprobe::AppendWorkerBarrier({});
    Expect(storage.sink.workerBarrierOverflow == 2,
           "worker barrier overflow saturated");
    Expect(storage.sink.workerBarrierCount ==
               storage.sink.workerBarrierCapacity,
           "a refused append consumed a barrier slot");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
#endif
}

void TestGenerationAndCallbackOwnership() {
    ProbeStorage storage;
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    harken::rtprobe::BeginDeviceStartAttempt();
    Expect(storage.sink.deviceGeneration == 1,
           "start attempt did not advance generation");
    Expect(harken::rtprobe::CoordinatorSink() == nullptr,
           "non-callback coordinator accepted");
    Expect(harken::rtprobe::ReserveTimeline(
               harken::rtprobe::TimelineKind::MixPublication) == nullptr,
           "non-callback publication writer accepted");
    Expect(storage.sink.timelineCount == 0,
           "rejected publication consumed a record");
    Expect(harken::rtprobe::EnterDeviceCallback(),
           "first callback writer refused");
    Expect(harken::rtprobe::RecordGenerationCallbackBegin(90),
           "first generation callback begin was not recorded");
    Expect(!harken::rtprobe::RecordGenerationCallbackBegin(91),
           "duplicate generation callback begin was accepted");
    harken::rtprobe::RecordGenerationCallbackEnd(true, 110);
    Expect(storage.sink.generationFirstCallbackBeginTicks.load(
               std::memory_order_relaxed) == 90,
           "first generation callback begin drifted");
    Expect(storage.sink.generationFirstCallbackEndTicks.load(
               std::memory_order_acquire) == 110,
           "first generation callback end drifted");
    Expect(harken::rtprobe::CoordinatorSink() == &storage.sink,
           "callback coordinator sink unavailable");
    auto* first = harken::rtprobe::ReserveTimeline(
        harken::rtprobe::TimelineKind::DeviceCallback);
    Expect(first != nullptr, "first callback record unavailable");
    if (first != nullptr) {
        harken::rtprobe::RecordCallbackArrival(*first, 100);
        harken::rtprobe::ValidateCoordinator(*first);
    }

    bool concurrentAccepted = true;
    std::thread concurrent([&] {
        concurrentAccepted = harken::rtprobe::EnterDeviceCallback();
        if (concurrentAccepted) {
            harken::rtprobe::LeaveDeviceCallback();
        }
    });
    concurrent.join();
    Expect(!concurrentAccepted, "concurrent callback writer accepted");
    Expect(storage.sink.concurrentCallbackCount.load(
               std::memory_order_relaxed) == 1,
           "concurrent callback not counted");
    harken::rtprobe::LeaveDeviceCallback();

    std::thread drift([&] {
        if (!harken::rtprobe::EnterDeviceCallback()) {
            return;
        }
        auto* record = harken::rtprobe::ReserveTimeline(
            harken::rtprobe::TimelineKind::DeviceCallback);
        if (record != nullptr) {
            harken::rtprobe::ValidateCoordinator(*record);
        }
        harken::rtprobe::LeaveDeviceCallback();
    });
    drift.join();
    Expect(storage.sink.identityMismatchCount == 1,
           "same-generation coordinator drift not detected");

    harken::rtprobe::BeginDeviceStartAttempt();
    Expect(storage.sink.generationFirstCallbackBeginTicks.load(
               std::memory_order_relaxed) == 0 &&
               storage.sink.generationFirstCallbackEndTicks.load(
                   std::memory_order_relaxed) == 0,
           "new generation retained prior callback ticks");
    const std::uint32_t mismatchBefore =
        storage.sink.identityMismatchCount;
    std::thread nextGeneration([&] {
        if (!harken::rtprobe::EnterDeviceCallback()) {
            return;
        }
        auto* record = harken::rtprobe::ReserveTimeline(
            harken::rtprobe::TimelineKind::DeviceCallback);
        if (record != nullptr) {
            harken::rtprobe::ValidateCoordinator(*record);
        }
        harken::rtprobe::LeaveDeviceCallback();
    });
    nextGeneration.join();
    Expect(storage.sink.deviceGeneration == 2,
           "second start attempt did not advance generation");
    Expect(storage.sink.identityMismatchCount == mismatchBefore,
           "new generation rejected a valid coordinator");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}

#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
void CountJob(void* userData, std::uint32_t) {
    auto* count = static_cast<std::atomic<std::uint32_t>*>(userData);
    count->fetch_add(1, std::memory_order_relaxed);
}

// Brings a pool to the state Run documents as its precondition: a created
// thread set with dispatch enabled, so WorkerCount() is non-zero and a batch
// really goes to the workers. Offline is the right mode here -- there is no
// device and no scheduling domain, and it must not be read as evidence about
// real-device deadline safety.
bool StartAndEnable(harken::RenderWorkerPool& pool) {
    if (!pool.Start(kProbeWorkerCount, kProbeJobCount)) {
        return false;
    }
    return pool.EnableOffline();
}

// Publishes one batch from a fresh thread holding the device-callback writer
// role, which is what production does: the coordinator IS the callback thread.
//
// This shape is mandatory once a device generation exists. CoordinatorSink only
// answers for the callback writer from that point on, so a batch published by
// any other thread records nothing at all -- and a case that dispatched outside
// a callback and then asserted "no drift was reported" would be passing on an
// empty barrier array, not on the behaviour it names.
//
// Each call runs on a new thread, so each one is a different coordinator
// identity. That is the point: same generation makes it drift, a new generation
// makes it a legitimate replacement.
void RunBatchAsCallbackThread(harken::RenderWorkerPool& pool,
                              std::atomic<std::uint32_t>& completed) {
    bool entered = false;
    std::thread callback([&] {
        entered = harken::rtprobe::EnterDeviceCallback();
        if (!entered) {
            return;
        }
        pool.Run(kProbeJobCount, &CountJob, &completed);
        harken::rtprobe::LeaveDeviceCallback();
    });
    callback.join();
    Expect(entered, "the callback writer role was refused");
}

// A batch that reached the worker threads and completed there: every job
// accounted for, every worker reported in at the barrier, and no degradation.
// Identity findings are reported separately in identityMismatchMask, so a
// mismatch does not disturb this.
bool BarrierRecordIsClean(
    const harken::rtprobe::WorkerBarrierRecord& record) {
    return record.jobCount == kProbeJobCount &&
           record.jobsCompleted == kProbeJobCount &&
           record.workerCount == kProbeWorkerCount &&
           record.workersFinished == kProbeWorkerCount &&
           record.flags == harken::rtprobe::kWorkerBarrierFlagNone;
}

void TestWorkerPoolUsesExistingBarrier() {
    ProbeStorage storage;
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    std::atomic<std::uint32_t> completed{0};
    harken::RenderWorkerPool pool;
    Expect(StartAndEnable(pool), "worker pool refused to dispatch");
    Expect(pool.WorkerCount() == kProbeWorkerCount,
           "enabled pool reported no batch participants");
    pool.Run(kProbeJobCount, &CountJob, &completed);
    const std::uint64_t firstToken0 = storage.workerTokens[0];
    const std::uint64_t firstToken1 = storage.workerTokens[1];
    Expect(firstToken0 != 0 && firstToken1 != 0,
           "worker identity was never recorded");
    Expect(firstToken0 != firstToken1,
           "two live workers reported one identity");
    pool.Run(kProbeJobCount, &CountJob, &completed);
    Expect(storage.workerTokens[0] == firstToken0 &&
               storage.workerTokens[1] == firstToken1,
           "worker identity changed across batches");
    storage.workerTokens[0] ^= std::uint64_t{1} << 32u;
    pool.Run(kProbeJobCount, &CountJob, &completed);
    pool.Stop();
    Expect(completed.load(std::memory_order_relaxed) == 3 * kProbeJobCount,
           "worker jobs were not completed exactly once");
    Expect(storage.sink.workerBarrierCount == 3,
           "worker barrier records missing");
    if (storage.sink.workerBarrierCount == 3) {
        const auto& record = storage.barriers[0];
        Expect(record.epoch == 1,
               "the first batch of a thread set did not publish epoch 1");
        Expect(record.jobsCompleted == kProbeJobCount,
                "worker record job count mismatch");
        Expect(record.workersFinished == kProbeWorkerCount,
               "worker completion barrier mismatch");
        Expect(record.flags == harken::rtprobe::kWorkerBarrierFlagNone,
                "worker record contains a fault");
        Expect(record.identityMismatchMask == 0,
               "a first-seen worker identity reported a mismatch");
        Expect(storage.barriers[1].identityMismatchMask == 0,
               "stable worker identity reported a mismatch");
        Expect((storage.barriers[2].identityMismatchMask & 1u) != 0,
               "induced worker identity mismatch was not reported");
        Expect((storage.barriers[2].identityMismatchMask & 2u) == 0,
               "an untouched worker identity was reported as mismatched");
        Expect(BarrierRecordIsClean(storage.barriers[2]),
               "an identity finding was reported as a dispatch fault");
    }
    Expect(storage.sink.workerCoordinatorMismatchCount == 0,
           "worker coordinator identity drifted");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}

void TestWorkerPoolRestartKeepsBarrierContract() {
    // The barrier, identity, and accounting contracts across a thread-set
    // replacement, driven through the lifecycle entry the engine uses rather
    // than by constructing a second pool: a resumed engine keeps the same pool
    // object and the same frozen worker count, and every batch after the
    // replacement has to be as measurable as the batches before it.
    ProbeStorage storage;
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    std::atomic<std::uint32_t> completed{0};
    harken::RenderWorkerPool pool;
    Expect(StartAndEnable(pool), "worker pool refused to dispatch");
    pool.Run(kProbeJobCount, &CountJob, &completed);

    const std::uint64_t retiredTokens[kProbeWorkerCount] = {
        storage.workerTokens[0], storage.workerTokens[1]};
    Expect(retiredTokens[0] != 0 && retiredTokens[1] != 0,
           "worker identity was never recorded");

    pool.Stop();
    Expect(pool.WorkerCount() == 0, "a stopped pool offered participants");
    Expect(pool.EnsureReady(), "replacement thread set refused");
    // A created set is deliberately not a dispatching set: the count stays zero
    // until dispatch is enabled, which is the distinction the original worker
    // case missed.
    Expect(pool.WorkerCount() == 0,
           "a created but disabled thread set offered participants");
    Expect(pool.EnableOffline(),
           "offline dispatch refused after the restart");
    Expect(pool.WorkerCount() == kProbeWorkerCount,
           "the replacement thread set is the wrong size");

    pool.Run(kProbeJobCount, &CountJob, &completed);
    Expect(storage.sink.workerBarrierCount == 2,
           "the batch after the restart was not recorded");
    if (storage.sink.workerBarrierCount == 2) {
        const auto& record = storage.barriers[1];
        Expect(record.epoch == 1,
               "the replacement thread set did not restart the epoch");
        Expect(BarrierRecordIsClean(record),
               "the batch after the restart did not complete on the workers");
        // The mask's rule, not a contingent outcome: bit i is set exactly when
        // the thread that wrote row i is not the one the token row already
        // named. Asserting "a restart always reports a mismatch" would be
        // wrong -- the platform may hand a retired thread id straight back to a
        // replacement -- so what is pinned is the correspondence itself.
        for (std::uint32_t i = 0; i < kProbeWorkerCount; ++i) {
            const bool reported =
                ((record.identityMismatchMask >> i) & 1u) != 0;
            const bool replaced =
                storage.scratch[i].writerToken != retiredTokens[i];
            Expect(reported == replaced,
                   "the identity mask disagreed with the observed writer");
        }
    }

    // The token row is how the tool asks "is the same thread still behind
    // worker slot i", so it has to be rebased whenever the threads behind those
    // slots are replaced. The facility owns that for the one boundary it can
    // see -- a device start attempt, covered separately below. This restart
    // crosses no device generation, so the tool rebases its own storage here,
    // and what the rest of this case pins is that a rebased row relearns and
    // still detects.
    storage.workerTokens[0] = 0;
    storage.workerTokens[1] = 0;
    pool.Run(kProbeJobCount, &CountJob, &completed);
    Expect(storage.sink.workerBarrierCount == 3,
           "the relearned-identity batch was not recorded");
    if (storage.sink.workerBarrierCount == 3) {
        Expect(storage.barriers[2].identityMismatchMask == 0,
               "a cleared token row still reported an identity mismatch");
        Expect(BarrierRecordIsClean(storage.barriers[2]),
               "clearing the token row disturbed the barrier accounting");
    }
    Expect(storage.workerTokens[0] != 0 && storage.workerTokens[1] != 0,
           "the cleared token row was never relearned");
    Expect(storage.workerTokens[0] != storage.workerTokens[1],
           "two live workers reported one identity after the restart");

    // Clearing the row must not blind the check the row exists for.
    storage.workerTokens[1] ^= std::uint64_t{1} << 32u;
    pool.Run(kProbeJobCount, &CountJob, &completed);
    pool.Stop();
    Expect(storage.sink.workerBarrierCount == 4,
           "the induced-mismatch batch was not recorded");
    if (storage.sink.workerBarrierCount == 4) {
        Expect((storage.barriers[3].identityMismatchMask & 2u) != 0,
               "an induced mismatch went unreported after the restart");
        Expect((storage.barriers[3].identityMismatchMask & 1u) == 0,
               "an untouched worker identity was reported as mismatched");
    }
    Expect(completed.load(std::memory_order_relaxed) == 4 * kProbeJobCount,
           "jobs were lost or repeated across the restart");
    // The publishing thread never changed. A worker thread set that did must
    // not be charged to the coordinator.
    Expect(storage.sink.workerCoordinatorMismatchCount == 0,
           "worker coordinator identity drifted across the restart");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}

void TestWorkerIdentityBaselineIsScopedToDeviceGeneration() {
    // The worker-barrier identity baselines belong to a device generation, the
    // same way the device-side coordinator token does. Both halves are pinned
    // here, and both are needed: without the first, a device restart reports
    // every later batch as drift; without the second, the fix would have been
    // indistinguishable from deleting the check.
    ProbeStorage storage;
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    std::atomic<std::uint32_t> completed{0};
    harken::RenderWorkerPool pool;
    Expect(StartAndEnable(pool), "worker pool refused to dispatch");

    harken::rtprobe::BeginDeviceStartAttempt();
    Expect(storage.sink.deviceGeneration == 1,
           "start attempt did not advance generation");
    RunBatchAsCallbackThread(pool, completed);
    Expect(storage.sink.workerBarrierCount == 1,
           "the first generation's batch was not recorded");
    const std::uint64_t firstCoordinator =
        storage.sink.workerCoordinatorToken;
    Expect(firstCoordinator != 0, "coordinator identity was never recorded");
    Expect(storage.sink.workerCoordinatorMismatchCount == 0,
           "the first coordinator of a generation was reported as drift");

    // Half one: a different callback thread inside the same generation is
    // exactly what the check exists to catch.
    RunBatchAsCallbackThread(pool, completed);
    Expect(storage.sink.workerBarrierCount == 2,
           "the same-generation replacement batch was not recorded");
    if (storage.sink.workerBarrierCount == 2) {
        Expect((storage.barriers[1].flags &
                harken::rtprobe::
                    kWorkerBarrierFlagCoordinatorIdentityMismatch) != 0,
               "same-generation coordinator drift not reported");
    }
    Expect(storage.sink.workerCoordinatorMismatchCount == 1,
           "same-generation coordinator drift not counted");

    // Forge a worker baseline as well, so the generation boundary below is
    // shown to rebase the worker rows and not only the coordinator token.
    storage.workerTokens[0] ^= std::uint64_t{1} << 32u;

    // Half two: crossing a generation boundary rebases the baselines.
    harken::rtprobe::BeginDeviceStartAttempt();
    Expect(storage.sink.deviceGeneration == 2,
           "second start attempt did not advance generation");
    Expect(storage.sink.workerCoordinatorToken == 0,
           "a new generation kept the previous coordinator baseline");
    Expect(storage.workerTokens[0] == 0 && storage.workerTokens[1] == 0,
           "a new generation kept the previous worker token rows");
    // The tally is evidence, not a baseline: drift seen in generation 1 really
    // happened, and a restart must not be able to clear the record of it.
    Expect(storage.sink.workerCoordinatorMismatchCount == 1,
           "a new generation erased drift already observed");

    RunBatchAsCallbackThread(pool, completed);
    Expect(storage.sink.workerBarrierCount == 3,
           "the new generation's batch was not recorded");
    if (storage.sink.workerBarrierCount == 3) {
        const auto& record = storage.barriers[2];
        Expect((record.flags &
                harken::rtprobe::
                    kWorkerBarrierFlagCoordinatorIdentityMismatch) == 0,
               "a new generation's callback thread was reported as drift");
        Expect(record.identityMismatchMask == 0,
               "a new generation kept a forged worker baseline");
        Expect(BarrierRecordIsClean(record),
               "the new generation's batch did not complete on the workers");
    }
    Expect(storage.sink.workerCoordinatorMismatchCount == 1,
           "a legitimate replacement coordinator was counted as drift");
    Expect(storage.sink.workerCoordinatorToken != 0,
           "the coordinator baseline was not relearned");
    Expect(storage.workerTokens[0] != 0 && storage.workerTokens[1] != 0,
           "the worker token rows were not relearned");

    // And the rebase did not blind either check: still inside generation 2, a
    // further replacement on both faces has to report again.
    storage.workerTokens[1] ^= std::uint64_t{1} << 32u;
    RunBatchAsCallbackThread(pool, completed);
    pool.Stop();
    Expect(storage.sink.workerBarrierCount == 4,
           "the post-rebase drift batch was not recorded");
    if (storage.sink.workerBarrierCount == 4) {
        const auto& record = storage.barriers[3];
        Expect((record.flags &
                harken::rtprobe::
                    kWorkerBarrierFlagCoordinatorIdentityMismatch) != 0,
               "coordinator drift went unreported after a generation rebase");
        Expect((record.identityMismatchMask & 2u) != 0,
               "worker drift went unreported after a generation rebase");
        Expect((record.identityMismatchMask & 1u) == 0,
               "an untouched worker identity was reported as mismatched");
    }
    Expect(storage.sink.workerCoordinatorMismatchCount == 2,
           "drift after a generation rebase was not counted");
    Expect(completed.load(std::memory_order_relaxed) == 4 * kProbeJobCount,
           "worker jobs were not completed exactly once");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}

void TestWorkerCoordinatorIdentityIsReported() {
    // The coordinator half of the identity check. The token names the thread
    // that publishes batches; a second publisher inside one measurement means
    // the records no longer describe one callback's timeline, so it has to
    // appear on the record and in the count rather than being absorbed.
    ProbeStorage storage;
    Expect(harken::rtprobe::Attach(&storage.sink), "valid sink refused");
    std::atomic<std::uint32_t> completed{0};
    harken::RenderWorkerPool pool;
    Expect(StartAndEnable(pool), "worker pool refused to dispatch");
    pool.Run(kProbeJobCount, &CountJob, &completed);
    Expect(storage.sink.workerCoordinatorToken != 0,
           "coordinator identity was never recorded");
    Expect(storage.sink.workerCoordinatorMismatchCount == 0,
           "the first coordinator was reported as drift");

    // Moved to another thread, not overlapped with this one: still exactly one
    // publisher at a time, which is what the pool requires.
    std::thread foreign(
        [&] { pool.Run(kProbeJobCount, &CountJob, &completed); });
    foreign.join();
    pool.Stop();

    Expect(storage.sink.workerBarrierCount == 2,
           "the foreign-coordinator batch was not recorded");
    if (storage.sink.workerBarrierCount == 2) {
        constexpr std::uint32_t kCoordinatorDrift =
            harken::rtprobe::kWorkerBarrierFlagCoordinatorIdentityMismatch;
        constexpr std::uint32_t kDispatchFaults =
            harken::rtprobe::kWorkerBarrierFlagWakeFault |
            harken::rtprobe::kWorkerBarrierFlagJobFault |
            harken::rtprobe::kWorkerBarrierFlagSerialFallback |
            harken::rtprobe::kWorkerBarrierFlagIncompleteScratch;
        const auto& record = storage.barriers[1];
        Expect((record.flags & kCoordinatorDrift) != 0,
               "a second coordinator was not reported on the record");
        Expect((record.flags & kDispatchFaults) == 0,
               "a coordinator finding was reported as a dispatch fault");
        Expect(record.workersFinished == kProbeWorkerCount &&
                   record.jobsCompleted == kProbeJobCount,
               "the foreign-coordinator batch lost barrier accounting");
        Expect(storage.barriers[0].coordinatorToken !=
                   record.coordinatorToken,
               "two publishing threads recorded one coordinator");
    }
    Expect(storage.sink.workerCoordinatorMismatchCount == 1,
           "a second coordinator was not counted");
    Expect(completed.load(std::memory_order_relaxed) == 2 * kProbeJobCount,
           "worker jobs were not completed exactly once");
    Expect(harken::rtprobe::Detach(&storage.sink), "detach refused");
}
#endif

} // namespace

int main() {
    TestAttachmentAndOverflow();
    TestCaptureMask();
    TestWorkerBarrierCapacity();
    TestGenerationAndCallbackOwnership();
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    TestWorkerPoolUsesExistingBarrier();
    TestWorkerPoolRestartKeepsBarrierContract();
    TestWorkerCoordinatorIdentityIsReported();
    TestWorkerIdentityBaselineIsScopedToDeviceGeneration();
#endif
    if (g_failures != 0) {
        std::cerr << g_failures << " probe test(s) failed\n";
        return 1;
    }
    std::cout << "RT probe tests passed\n";
    return 0;
}
