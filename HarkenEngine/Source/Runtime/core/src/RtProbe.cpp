#include "harken/core/RtProbe.h"

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)

#include "harken/core/CpuClock.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstring>
#include <pthread.h>
#endif
#include <limits>

namespace harken::rtprobe {

namespace {

Sink* g_sink = nullptr;
thread_local bool g_deviceCallbackWriter = false;

bool CaptureEnabled(const Sink* sink, std::uint32_t bit) noexcept {
    return sink != nullptr &&
           (sink->captureMask == 0 || (sink->captureMask & bit) != 0);
}

} // namespace

bool Attach(Sink* sink) noexcept {
    if (sink == nullptr || g_sink != nullptr) {
        return false;
    }
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    if ((CaptureEnabled(sink, kCaptureDeviceCallback) ||
         CaptureEnabled(sink, kCaptureMixPublication)) &&
        (sink->timeline == nullptr || sink->timelineCapacity == 0)) {
        return false;
    }
#endif
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    if (CaptureEnabled(sink, kCaptureWorkerBarrier) &&
        (sink->workerBarriers == nullptr ||
        sink->workerBarrierCapacity == 0 ||
        sink->workerScratch == nullptr || sink->workerTokens == nullptr ||
        sink->workerScratchCapacity == 0)) {
        return false;
    }
#endif
    sink->timelineCount = 0;
    sink->timelineOverflow = 0;
    sink->workerBarrierCount = 0;
    sink->workerBarrierOverflow = 0;
    sink->tickRate = CpuClockTicksPerSecond();
    sink->measurementStartTicks = 0;
    sink->measurementEndTicks = 0;
    sink->coordinatorToken = 0;
    sink->deviceGeneration = 0;
    sink->coordinatorGeneration = 0;
    sink->identityMismatchCount = 0;
    sink->generationOverflowCount = 0;
    sink->lastCallbackArrivalTicks = 0;
    sink->workerCoordinatorToken = 0;
    sink->workerCoordinatorMismatchCount = 0;
    sink->callbackActive.clear(std::memory_order_relaxed);
    sink->concurrentCallbackCount.store(0, std::memory_order_relaxed);
    sink->generationFirstCallbackBeginTicks.store(
        0, std::memory_order_relaxed);
    sink->generationFirstCallbackEndTicks.store(
        0, std::memory_order_relaxed);
    sink->backend = BackendObservation{};
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    for (std::uint32_t i = 0; i < sink->workerScratchCapacity; ++i) {
        sink->workerScratch[i] = WorkerScratch{};
        sink->workerTokens[i] = 0;
    }
#endif
    g_sink = sink;
    return true;
}

bool Detach(Sink* sink) noexcept {
    if (g_sink != sink ||
        sink->callbackActive.test(std::memory_order_acquire)) {
        return false;
    }
    g_sink = nullptr;
    return true;
}

Sink* ActiveSink() noexcept {
    return g_sink;
}

Sink* CoordinatorSink() noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr ||
        (sink->deviceGeneration != 0 && !g_deviceCallbackWriter)) {
        return nullptr;
    }
    return sink;
}

Sink* WorkerBarrierSink() noexcept {
    Sink* sink = CoordinatorSink();
    return CaptureEnabled(sink, kCaptureWorkerBarrier) ? sink : nullptr;
}

std::uint64_t CurrentThreadToken() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentThreadId());
#else
    static_assert(sizeof(pthread_t) <= sizeof(std::uint64_t));
    const pthread_t thread = pthread_self();
    std::uint64_t token = 0;
    std::memcpy(&token, &thread, sizeof(thread));
    return token;
#endif
}

bool EnterDeviceCallback() noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr) {
        return false;
    }
    if (sink->callbackActive.test_and_set(std::memory_order_acquire)) {
        sink->concurrentCallbackCount.fetch_add(
            1, std::memory_order_relaxed);
        g_deviceCallbackWriter = false;
        return false;
    }
    g_deviceCallbackWriter = true;
    return true;
}

void LeaveDeviceCallback() noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr || !g_deviceCallbackWriter) {
        return;
    }
    g_deviceCallbackWriter = false;
    sink->callbackActive.clear(std::memory_order_release);
}

bool RecordGenerationCallbackBegin(std::uint64_t beginTicks) noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr || !g_deviceCallbackWriter) {
        return false;
    }
    std::uint64_t expected = 0;
    return sink->generationFirstCallbackBeginTicks.compare_exchange_strong(
        expected, beginTicks, std::memory_order_relaxed,
        std::memory_order_relaxed);
}

void RecordGenerationCallbackEnd(bool generationFirst,
                                 std::uint64_t endTicks) noexcept {
    Sink* sink = g_sink;
    if (sink != nullptr && g_deviceCallbackWriter && generationFirst) {
        sink->generationFirstCallbackEndTicks.store(
            endTicks, std::memory_order_release);
    }
}

void RecordCallbackArrival(TimelineRecord& record,
                           std::uint64_t arrivalTicks) noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr) {
        return;
    }
    record.priorArrivalTicks = sink->lastCallbackArrivalTicks;
    sink->lastCallbackArrivalTicks = arrivalTicks;
}

std::uint64_t PackStaging(std::uint32_t readFrames,
                          std::uint32_t availFrames) noexcept {
    return (static_cast<std::uint64_t>(readFrames) << 32u) |
           static_cast<std::uint64_t>(availFrames);
}

TimelineRecord* ReserveTimeline(TimelineKind kind) noexcept {
    Sink* sink = g_sink;
    const std::uint32_t captureBit =
        kind == TimelineKind::DeviceCallback
            ? kCaptureDeviceCallback
            : kCaptureMixPublication;
    if (!CaptureEnabled(sink, captureBit)) {
        return nullptr;
    }
    if (kind == TimelineKind::MixPublication &&
        CoordinatorSink() == nullptr) {
        return nullptr;
    }
    if (sink == nullptr || sink->timeline == nullptr ||
        sink->timelineCount >= sink->timelineCapacity) {
        if (sink != nullptr) {
            ++sink->timelineOverflow;
        }
        return nullptr;
    }
    TimelineRecord* record =
        &sink->timeline[sink->timelineCount++];
    *record = TimelineRecord{};
    record->sequence = sink->timelineCount - 1u;
    record->kind = static_cast<std::uint16_t>(kind);
    return record;
}

void ValidateCoordinator(TimelineRecord& record) noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr) {
        return;
    }
    const std::uint64_t token = CurrentThreadToken();
    record.writerToken = token;
    const std::uint32_t generation = sink->deviceGeneration;
    record.aux1 = static_cast<std::uint16_t>(generation);
    if (generation >
        (std::numeric_limits<std::uint16_t>::max)()) {
        record.flags |= kTimelineFlagGenerationOverflow;
    }
    if (sink->coordinatorToken == 0 ||
        sink->coordinatorGeneration != generation) {
        sink->coordinatorToken = token;
        sink->coordinatorGeneration = generation;
    } else if (sink->coordinatorToken != token) {
        record.flags |= kTimelineFlagWriterIdentityMismatch;
        ++sink->identityMismatchCount;
    }
}

void BeginDeviceStartAttempt() noexcept {
    Sink* sink = g_sink;
    if (sink == nullptr) {
        return;
    }
    if (sink->deviceGeneration ==
        (std::numeric_limits<std::uint32_t>::max)()) {
        ++sink->generationOverflowCount;
        return;
    }
    ++sink->deviceGeneration;
    sink->generationFirstCallbackBeginTicks.store(
        0, std::memory_order_relaxed);
    sink->generationFirstCallbackEndTicks.store(
        0, std::memory_order_relaxed);
    sink->coordinatorToken = 0;
    sink->coordinatorGeneration = sink->deviceGeneration;
    sink->lastCallbackArrivalTicks = 0;
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    // A start attempt brings up a new callback thread and a render worker set
    // to go with it, so every identity baseline recorded against the previous
    // generation now names a thread that no longer exists. Clearing them is
    // what lets the next batch learn who the coordinator and the workers are
    // this time. Left standing, they turn a legitimate replacement into drift
    // reported on every remaining batch of the session, which is the same
    // signal a real impostor produces -- so the check stops distinguishing the
    // two exactly when a device restart makes the distinction matter.
    //
    // Baselines only. The mismatch tallies survive, because drift observed in
    // an earlier generation did happen and a later restart must not erase the
    // evidence; that is the split the device side above already makes, where
    // coordinatorToken is cleared and identityMismatchCount is not.
    //
    // The per-worker scratch rows are deliberately not cleared here. They are
    // not identity baselines, and a batch zeroes the rows of its own live
    // workers before it publishes, so no row a coordinator reads can carry a
    // previous generation's contents.
    //
    // Writing worker-owned storage is legal at this point for the same reason
    // Attach may write it: the caller is on the lifecycle thread on the device
    // start path, before the callback runs, so no batch is in flight and no
    // worker is inside one.
    sink->workerCoordinatorToken = 0;
    for (std::uint32_t i = 0; i < sink->workerScratchCapacity; ++i) {
        sink->workerTokens[i] = 0;
    }
#endif
}

void RecordBackendObservation(
    const BackendObservation& observation) noexcept {
    Sink* sink = g_sink;
    if (sink != nullptr) {
        sink->backend = observation;
    }
}

void AppendWorkerBarrier(const WorkerBarrierRecord& input) noexcept {
    Sink* sink = g_sink;
    if (!CaptureEnabled(sink, kCaptureWorkerBarrier)) {
        return;
    }
    if (sink == nullptr || sink->workerBarriers == nullptr ||
        sink->workerBarrierCount >= sink->workerBarrierCapacity) {
        if (sink != nullptr) {
            ++sink->workerBarrierOverflow;
        }
        return;
    }
    WorkerBarrierRecord record = input;
    record.sequence = sink->workerBarrierCount;
    sink->workerBarriers[sink->workerBarrierCount++] = record;
}

} // namespace harken::rtprobe

#endif
