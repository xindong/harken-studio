// Private real-time qualification probes. Shipping Runtime targets do not
// define any HARKEN_RT_PROBE_* control and therefore compile every call and all
// storage out. A qualification-only static Runtime receives a tool-owned sink
// before Vn_Init; the tool keeps the storage alive until Vn_Term has stopped
// the device callback and joined render workers.
#ifndef HARKEN_CORE_RT_PROBE_H
#define HARKEN_CORE_RT_PROBE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace harken::rtprobe {

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
inline constexpr bool kAnyEnabled = true;
#else
inline constexpr bool kAnyEnabled = false;
#endif

enum class TimelineKind : std::uint16_t {
    DeviceCallback = 1,
    MixPublication = 2,
};

enum CaptureMask : std::uint32_t {
    kCaptureDeviceCallback = 1u << 0,
    kCaptureMixPublication = 1u << 1,
    kCaptureWorkerBarrier = 1u << 2,
    kCaptureAll = kCaptureDeviceCallback | kCaptureMixPublication |
                  kCaptureWorkerBarrier,
};

enum TimelineFlags : std::uint16_t {
    kTimelineFlagNone = 0,
    kTimelineFlagWriterIdentityMismatch = 1u << 0,
    kTimelineFlagGenerationOverflow = 1u << 1,
    kTimelineFlagMixCountOverflow = 1u << 2,
    kTimelineFlagMeterArmed = 1u << 3,
    kTimelineFlagSourceCursorPublished = 1u << 4,
};

// Fixed-size coordinator record. Device callback records pack
// readFrames/availFrames into logicalIndex and relatedIndex. Mix publication
// records use logicalIndex for blockIndex and flags for enabled publications.
struct TimelineRecord {
    std::uint64_t sequence = 0;
    std::uint64_t beginTicks = 0;
    std::uint64_t endTicks = 0;
    std::uint64_t logicalIndex = 0;
    std::uint64_t relatedIndex = 0;
    std::uint64_t writerToken = 0;
    std::uint64_t priorArrivalTicks = 0;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint16_t kind = 0;
    std::uint16_t flags = 0;
    std::uint16_t aux0 = 0;
    std::uint16_t aux1 = 0;
};
static_assert(sizeof(TimelineRecord) == 72);

enum WorkerBarrierFlags : std::uint32_t {
    kWorkerBarrierFlagNone = 0,
    kWorkerBarrierFlagWakeFault = 1u << 0,
    kWorkerBarrierFlagJobFault = 1u << 1,
    kWorkerBarrierFlagSerialFallback = 1u << 2,
    kWorkerBarrierFlagIncompleteScratch = 1u << 3,
    kWorkerBarrierFlagCoordinatorIdentityMismatch = 1u << 4,
};

struct WorkerBarrierRecord {
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint64_t publishTicks = 0;
    std::uint64_t coordinatorDoneTicks = 0;
    std::uint64_t firstWorkerRunTicks = 0;
    std::uint64_t lastWorkerRunTicks = 0;
    std::uint64_t lastWorkerDoneTicks = 0;
    std::uint64_t barrierDoneTicks = 0;
    std::uint64_t coordinatorToken = 0;
    std::uint64_t yieldIterations = 0;
    std::uint64_t identityMismatchMask = 0;
    std::uint32_t jobCount = 0;
    std::uint32_t jobsCompleted = 0;
    std::uint32_t workerCount = 0;
    std::uint32_t workersFinished = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(WorkerBarrierRecord) == 112);

enum WorkerScratchFlags : std::uint32_t {
    kWorkerScratchFlagNone = 0,
    kWorkerScratchFlagIdentityMismatch = 1u << 0,
    kWorkerScratchFlagEpochMismatch = 1u << 1,
};

// Worker-owned scratch for one in-flight batch. Each worker writes only its
// own cache line before the existing completion release; the coordinator reads
// the rows only after the existing acquire barrier.
struct alignas(64) WorkerScratch {
    std::uint64_t writerToken = 0;
    std::uint64_t observedEpoch = 0;
    std::uint64_t runBeginTicks = 0;
    std::uint64_t doneTicks = 0;
    std::uint32_t jobsCompleted = 0;
    std::uint32_t flags = 0;
    std::uint8_t reserved[24]{};
};
static_assert(sizeof(WorkerScratch) == 64);

struct BackendObservation {
    std::uint32_t valid = 0;
    std::uint32_t backendCode = 0;
    std::uint32_t clientSampleRate = 0;
    std::uint32_t clientChannels = 0;
    std::uint32_t clientFormat = 0;
    std::uint32_t requestedPeriodFrames = 0;
    std::uint32_t requestedPeriods = 0;
    std::uint32_t internalSampleRate = 0;
    std::uint32_t internalChannels = 0;
    std::uint32_t internalFormat = 0;
    std::uint32_t internalPeriodFrames = 0;
    std::uint32_t internalPeriods = 0;
    // These remain zero until an external platform oracle supplies them.
    std::uint32_t endpointSampleRate = 0;
    std::uint32_t conversionOwner = 0;
    std::uint64_t deviceIdHash = 0;
    char backendName[32]{};
    char deviceName[256]{};
};

enum BackendConversionObservation : std::uint32_t {
    kBackendConversionUnknown = 0,
    kBackendConversionNoClientStageObserved = 1,
    kBackendConversionMiniaudioClientStageObserved = 2,
};

// Owned by the qualification tool, not by Engine. Timeline and barrier records
// are coordinator-written. Each worker owns one scratch row and token slot;
// the coordinator reads them only after the existing acquire barrier. The tool
// reads all storage only after Vn_Term.
struct Sink {
    TimelineRecord* timeline = nullptr;
    std::uint32_t timelineCapacity = 0;
    std::uint32_t timelineCount = 0;
    std::uint64_t timelineOverflow = 0;

    WorkerBarrierRecord* workerBarriers = nullptr;
    std::uint32_t workerBarrierCapacity = 0;
    std::uint32_t workerBarrierCount = 0;
    std::uint64_t workerBarrierOverflow = 0;

    WorkerScratch* workerScratch = nullptr;
    std::uint64_t* workerTokens = nullptr;
    std::uint32_t workerScratchCapacity = 0;
    // Zero preserves the original all-probes behavior. Qualification tools
    // may select a bounded subset before Attach; the mask is immutable while
    // attached.
    std::uint32_t captureMask = 0;

    std::uint64_t tickRate = 0;
    std::uint64_t measurementStartTicks = 0;
    std::uint64_t measurementEndTicks = 0;
    std::uint64_t coordinatorToken = 0;
    std::uint32_t deviceGeneration = 0;
    std::uint32_t coordinatorGeneration = 0;
    std::uint32_t identityMismatchCount = 0;
    std::uint32_t generationOverflowCount = 0;
    std::uint64_t lastCallbackArrivalTicks = 0;
    // Worker-barrier identity: the token is the baseline ("who published the
    // batches of this device generation") and the count is the cumulative
    // tally of batches published by anyone else. A start attempt clears the
    // baseline so a replacement callback thread is not read as drift, and
    // deliberately leaves the tally alone so drift already observed is not
    // erased by a later restart. Same split as coordinatorToken against
    // identityMismatchCount above.
    std::uint64_t workerCoordinatorToken = 0;
    std::uint32_t workerCoordinatorMismatchCount = 0;
    std::atomic_flag callbackActive = ATOMIC_FLAG_INIT;
    std::atomic<std::uint32_t> concurrentCallbackCount{0};
    std::atomic<std::uint64_t> generationFirstCallbackBeginTicks{0};
    std::atomic<std::uint64_t> generationFirstCallbackEndTicks{0};
    BackendObservation backend;
};

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)

bool Attach(Sink* sink) noexcept;
bool Detach(Sink* sink) noexcept;
Sink* ActiveSink() noexcept;
Sink* CoordinatorSink() noexcept;
Sink* WorkerBarrierSink() noexcept;
std::uint64_t CurrentThreadToken() noexcept;
bool EnterDeviceCallback() noexcept;
void LeaveDeviceCallback() noexcept;
bool RecordGenerationCallbackBegin(std::uint64_t beginTicks) noexcept;
void RecordGenerationCallbackEnd(bool generationFirst,
                                 std::uint64_t endTicks) noexcept;
void RecordCallbackArrival(TimelineRecord& record,
                           std::uint64_t arrivalTicks) noexcept;
std::uint64_t PackStaging(std::uint32_t readFrames,
                          std::uint32_t availFrames) noexcept;
TimelineRecord* ReserveTimeline(TimelineKind kind) noexcept;
void ValidateCoordinator(TimelineRecord& record) noexcept;
// Opens a new device generation: advances the counter, clears the
// per-generation callback ticks, and clears every writer-identity baseline --
// the device coordinator token, the worker coordinator token, and the
// per-worker token rows. Mismatch tallies are cumulative across generations
// and are not touched. Lifecycle thread, on the device start path before the
// callback is running: it writes rows the workers own, so no batch may be in
// flight.
void BeginDeviceStartAttempt() noexcept;
void RecordBackendObservation(
    const BackendObservation& observation) noexcept;
void AppendWorkerBarrier(const WorkerBarrierRecord& record) noexcept;

#endif

} // namespace harken::rtprobe

#endif // HARKEN_CORE_RT_PROBE_H
