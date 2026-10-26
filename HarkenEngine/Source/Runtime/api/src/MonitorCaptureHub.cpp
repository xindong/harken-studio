#include "MonitorCaptureHub.h"
#include "MonitorCaptureHubTestHooks.h"

#include "Engine.h"
#include "MonitorDerivedDispatch.h"
#include "MonitorProducerRing.h"
#include "MonitorExecutionCodec.h"
#include "MonitorLifecycleProjection.h"
#include "MonitorExecutionProducer.h"
#include "MonitorIdentitySnapshot.h"
#include "MonitorIdentityState.h"
#include "MonitorIdentityCut.h"
#include "MonitorRecordPack.h"
#include "MonitorSessionFileWrite.h"
#include "MonitorRecordingCodec.h"

#include "harken/core/AllocGuard.h"
#include "harken/core/LogRing.h"
#include "harken/core/MonitorIdentity.h"
#include "harken/core/VariableExecutionFingerprint.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/MonitorMeterReduce.h"
#include "harken/core/MonitorSpectrumFft.h"
#include "harken/core/Utf8Path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

namespace harken::monitor {

// Bits follow allowlist_monitor_functions.json / public Vn_Monitor* order.
constexpr std::uint32_t kImplementedApiMask =
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
    (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
    (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) |
    (1u << 17) | (1u << 18);
constexpr std::uint32_t kCaptureControlPermission = VN_MONITOR_PERM_CAPTURE_CONTROL;
constexpr std::uint32_t kNormalPlanSlots = VN_MONITOR_DEFAULT_PLAN_SLOTS;
constexpr std::uint32_t kStopSlotIndex = VN_MONITOR_DEFAULT_PLAN_SLOTS;
constexpr std::uint32_t kTotalPlanSlots =
    VN_MONITOR_DEFAULT_PLAN_SLOTS + VN_MONITOR_DEDICATED_STOP_SLOTS;
constexpr std::uint32_t kLeaseReasonNone = 0;
constexpr std::uint32_t kLeaseReasonAcquire = 1;
constexpr std::uint32_t kLeaseReasonRelease = 2;
constexpr std::uint32_t kLeaseReasonExpiry = 3;
constexpr std::uint32_t kLeaseReasonDisconnect = 4;
constexpr std::uint32_t kPlanReasonApply = VN_MONITOR_EVENT_PLAN;
constexpr std::uint32_t kPlanReasonStop = VN_MONITOR_EVENT_STOP;

enum class SlotOccupancy : std::uint32_t {
    Free = 0,
    Pending = 1,
    Active = 2,
    Lingering = 3,
    Quarantine = 4
};

struct PlanSlot {
    SlotOccupancy occupancy = SlotOccupancy::Free;
    bool empty = true;
    bool quarantined = false;
    VnMonitorCapturePlan plan{};
    std::array<VnMonitorTapDescriptor, VN_MONITOR_HARD_TAPS_PER_PLAN> taps{};
    std::array<VnMonitorMeasurementRequest, VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN>
        measurements{};
    std::uint32_t storedTapCount = 0;
    std::uint32_t storedMeasurementCount = 0;
    std::shared_ptr<DerivedDrainState> derivedState;
};

struct LeaseState {
    std::uint64_t generation = 0;
    bool held = false;
    VnMonitorSessionHandle holder = 0;
    std::uint64_t principalHash = 0;
    std::uint32_t permissions = 0;
    std::uint64_t expiryExecuteFrame = 0;
    std::uint32_t lastChangeReason = kLeaseReasonNone;
};
constexpr std::uint32_t kObservePermissionMask =
    VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_EVENTS |
    VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
constexpr std::uint32_t kRecordPermission = VN_MONITOR_PERM_RECORD;
constexpr std::uint32_t kObserveStreamMask =
    VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT |
    VN_MONITOR_STREAM_TIME_SERIES | VN_MONITOR_STREAM_OPTIONAL_AUDIO;

constexpr std::uint32_t kSlotBits = 8;
constexpr std::uint64_t kSlotMask = (1ull << kSlotBits) - 1ull;

VnMonitorSessionHandle PackHandle(std::uint32_t slot, std::uint64_t generation) {
    return (generation << kSlotBits) | slot;
}

std::uint32_t HandleSlot(VnMonitorSessionHandle handle) {
    return static_cast<std::uint32_t>(handle & kSlotMask);
}

std::uint64_t HandleGeneration(VnMonitorSessionHandle handle) {
    return handle >> kSlotBits;
}

bool ValidStructSize(std::uint32_t structSize, std::uint32_t expected) {
    return structSize == expected;
}

bool ReservedZero(std::uint32_t reserved) { return reserved == 0; }

constexpr std::uint32_t kInternalRecorder = 0;
constexpr std::uint32_t kInternalRemote0 = 1;
constexpr std::uint32_t kInternalRemote1 = 2;
constexpr std::uint32_t kInternalConsumerCount = 3;
constexpr std::uint32_t kScratchBytes = 4096;
constexpr std::uint32_t kLiveTapSlots = VN_MONITOR_HARD_TAPS_PER_PLAN;
constexpr std::uint32_t kAudioSampleFormatF32 = 1;
constexpr std::uint32_t kInterleaveScratchFloats =
    VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES * kPackedAudioMaxChannels;

struct DropAccum {
    std::uint32_t stage = 0;
    std::uint64_t streamId = 0;
    std::uint64_t firstSourceSequence = 0;
    std::uint64_t lastSourceSequence = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t byteCount = 0;
    std::uint32_t reason = 0;

    void Clear() {
        firstSourceSequence = 0;
        lastSourceSequence = 0;
        recordCount = 0;
        byteCount = 0;
        reason = 0;
    }

    void Note(std::uint64_t sourceSequence, std::uint64_t bytes,
              std::uint32_t dropReason) {
        NoteRange(sourceSequence, sourceSequence, 1, bytes, dropReason);
    }

    void NoteRange(std::uint64_t first, std::uint64_t last, std::uint64_t count,
                   std::uint64_t bytes, std::uint32_t dropReason) {
        if (recordCount == 0) {
            firstSourceSequence = first;
        }
        lastSourceSequence = last;
        recordCount = UINT64_MAX - recordCount < count ? UINT64_MAX : recordCount + count;
        byteCount = UINT64_MAX - byteCount < bytes ? UINT64_MAX : byteCount + bytes;
        reason = dropReason;
        if (streamId == 0) {
            streamId = kSyntheticStreamId;
        }
    }
};

// Single-writer (audio publish) / single-snapshot (logic GetConsumerStatus)
// drop accumulator. Publish never takes a mutex. Snapshot copies the packed
// word with acquire so range/count/bytes are consistent.
struct AtomicDropAccum {
    std::atomic<std::uint32_t> stage{0};
    std::atomic<std::uint64_t> streamId{0};
    std::atomic<std::uint64_t> firstSourceSequence{0};
    std::atomic<std::uint64_t> lastSourceSequence{0};
    std::atomic<std::uint64_t> recordCount{0};
    std::atomic<std::uint64_t> byteCount{0};
    std::atomic<std::uint32_t> reason{0};
    std::atomic<std::uint32_t> seq{0};

    void Clear() {
        const std::uint32_t begin = seq.fetch_add(1, std::memory_order_acq_rel);
        (void)begin;
        firstSourceSequence.store(0, std::memory_order_relaxed);
        lastSourceSequence.store(0, std::memory_order_relaxed);
        byteCount.store(0, std::memory_order_relaxed);
        reason.store(0, std::memory_order_relaxed);
        recordCount.store(0, std::memory_order_release);
        seq.fetch_add(1, std::memory_order_release);
    }

    void Note(std::uint64_t sourceSequence, std::uint64_t bytes,
              std::uint32_t dropReason) {
        seq.fetch_add(1, std::memory_order_acq_rel);
        if (streamId.load(std::memory_order_relaxed) == 0) {
            streamId.store(kSyntheticStreamId, std::memory_order_relaxed);
        }
        if (recordCount.load(std::memory_order_relaxed) == 0) {
            firstSourceSequence.store(sourceSequence, std::memory_order_relaxed);
        }
        lastSourceSequence.store(sourceSequence, std::memory_order_relaxed);
        byteCount.fetch_add(bytes, std::memory_order_relaxed);
        reason.store(dropReason, std::memory_order_relaxed);
        recordCount.fetch_add(1, std::memory_order_release);
        seq.fetch_add(1, std::memory_order_release);
    }

    DropAccum Snapshot() const {
        DropAccum out;
        for (;;) {
            const std::uint32_t before = seq.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                continue;
            }
            out.stage = stage.load(std::memory_order_relaxed);
            out.streamId = streamId.load(std::memory_order_relaxed);
            out.firstSourceSequence =
                firstSourceSequence.load(std::memory_order_relaxed);
            out.lastSourceSequence =
                lastSourceSequence.load(std::memory_order_relaxed);
            out.recordCount = recordCount.load(std::memory_order_relaxed);
            out.byteCount = byteCount.load(std::memory_order_relaxed);
            out.reason = reason.load(std::memory_order_relaxed);
            const std::uint32_t after = seq.load(std::memory_order_acquire);
            if (before == after && (after & 1u) == 0) {
                return out;
            }
        }
    }
};

struct PackedRecord {
    std::vector<std::uint8_t> bytes;
    std::uint64_t sourceSequence = 0;
    std::uint32_t recordKind = 0;
    MonitorProducerOrigin origin{};
};

struct ConsumerQueue {
    std::uint64_t budgetBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t nextDelivery = 1;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    std::deque<PackedRecord> records;
    std::deque<PackedRecord> deferredSymbols;
    std::uint32_t symbolStreamKind = VN_MONITOR_STREAM_EVENT;
    bool symbolBaselineActive = false;
    // Every typed producer has an independent source sequence.
    static constexpr std::size_t kDropStreamCount = 6;
    std::array<DropAccum, kDropStreamCount> drops{};
    std::uint64_t droppedRecords = 0;
    std::array<std::uint64_t, kDropStreamCount> cumulativeDrops{};
    bool recordingReceiver = false;
    std::mutex mutex;

    void Reset(std::uint64_t budget, std::uint32_t stage, std::uint64_t newEpoch,
               std::uint64_t newGeneration, std::uint32_t symbolKind = VN_MONITOR_STREAM_EVENT) {
        std::lock_guard<std::mutex> lock(mutex);
        budgetBytes = budget;
        usedBytes = 0;
        nextDelivery = 1;
        epoch = newEpoch;
        generation = newGeneration;
        symbolStreamKind = symbolKind;
        records.clear();
        deferredSymbols.clear();
        symbolBaselineActive = false;
        for (auto& drop : drops) drop.Clear();
        droppedRecords = 0;
        cumulativeDrops = {};
        recordingReceiver = false;
        for (auto& drop : drops) drop.stage = stage;
        drops[0].streamId = kSyntheticStreamId;
        drops[1].streamId = kExecutionLogicStream;
        drops[2].streamId = kExecutionAudioStream;
        drops[3].streamId = kIdentityDeltaStream;
        drops[4].streamId = kIdentityBaselineStream;
        drops[5].streamId = kHostSymbolStream;
    }

    void Detach() {
        std::lock_guard<std::mutex> lock(mutex);
        records.clear();
        deferredSymbols.clear();
        symbolBaselineActive = false;
        usedBytes = 0;
        for (auto& drop : drops) drop.Clear();
        epoch = 0;
        generation = 0;
    }
};

struct HostPrincipal {
    bool authenticated = false;
    std::uint32_t observeMask = 0;
};

thread_local HostPrincipal t_hostPrincipal{};
thread_local bool t_hostPrincipalOverride = false;

HostPrincipal DefaultHostPrincipal() {
    HostPrincipal principal;
    principal.authenticated = true;
    principal.observeMask = kObserveStreamMask;
    return principal;
}

HostPrincipal CurrentHostPrincipal() {
    if (t_hostPrincipalOverride) {
        return t_hostPrincipal;
    }
    return DefaultHostPrincipal();
}

struct StateCut {
    std::uint64_t id = 0;
    std::uint32_t result = VN_MONITOR_STATE_CUT_ABORTED;
    std::uint32_t emittedCount = 0;
    std::uint64_t checksum = 0;
    std::uint64_t beginSourceSequence = 0;
    std::uint64_t upsertSourceSequence = 0;
    std::uint64_t endSourceSequence = 0;
    bool beginEnqueued = false;
    bool upsertEnqueued = false;
    bool endEnqueued = false;
    bool beginDelivered = false;
    bool endDelivered = false;
    bool settled = false;
};

struct IdentityStaging {
    std::vector<MonitorExecutionFact> rows;
    std::vector<MonitorHostSymbolValue> symbols;
    std::uint32_t identityCount = 0;
    VnMonitorIdentityCutBeginV1 begin{};
    std::uint64_t engine = 0;
    std::uint64_t firstSequence = 0;
    std::uint64_t checksum = kIdentityChecksumSeed;
    std::uint64_t emitted[9]{};
    std::uint32_t count = 0;
    std::uint32_t cursor = 0;
    bool active = false;
    bool sourceComplete = false;
    bool requested = false;
    MonitorIdentityCutVerifier delivered;
};

bool IsIdentityRecord(std::uint32_t kind) {
    return kind == VN_MONITOR_RECORD_IDENTITY_FACT || kind == VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN ||
        kind == VN_MONITOR_RECORD_IDENTITY_CUT_END || kind == VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL;
}

struct Session {
    std::atomic<bool> live{false};
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::atomic<std::uint32_t> requestedStreamKindMask{0};
    std::uint32_t requestedPermissionMask = 0;
    std::uint32_t acceptedStreamKindMask = 0;
    std::uint32_t acceptedPermissionMask = 0;
    std::uint32_t deniedPermissionMask = 0;
    std::uint32_t unsupportedPermissionMask = 0;
    std::uint32_t requestRawAudio = 0;
    std::uint32_t requestSymbols = 0;
    std::uint32_t rawAudioDecision = 0;
    std::uint32_t symbolsDecision = 0;
    std::uint32_t executionFactVersion = 0;
    std::array<std::uint64_t, 2> supportedFactKinds{};
    std::uint32_t baselineAvailability = VN_MONITOR_OBSERVATION_NOT_REQUESTED;
    std::uint32_t symbolAvailability = VN_MONITOR_OBSERVATION_NOT_REQUESTED;
    std::uint64_t principalHash = 0;
    std::uint64_t consumerQueueBytes = 0;
    std::uint64_t firstExpectedTimeSeriesSequence = 0;
    std::uint64_t firstExpectedStateSequence = 0;
    std::atomic<std::uint32_t> pollInFlight{0};
    std::atomic<bool> resyncRequired{false};
    std::atomic<bool> streamTerminated{false};
    std::atomic<ConsumerQueue*> queuePtr{nullptr};
    std::unique_ptr<ConsumerQueue> queue;
    StateCut cut{};
    IdentityStaging identity;
    std::uint64_t identityStart = 0;
    std::uint64_t identityEnd = 0;
    DropAccum identityDrops{};
    DropAccum baselineDrops{};
    std::uint64_t symbolStart = 0, symbolEnd = 0;
    std::array<std::uint64_t, 2> symbolUnavailableStart{}, symbolUnavailableEnd{};
    DropAccum symbolDrops{};
    bool sealRequested = false;
    bool eventCatalog = false;
    bool recordingReceiver = false;
    bool identityInterestReleased = false;
    std::array<std::uint64_t, 2> ringStarts{}, ringEnds{}, logStarts{}, nativeLogDrops{};
    std::array<std::uint64_t, 2> logFiltered{}, logLost{}, logDownstreamEnds{};
    std::array<std::uint64_t, 2> rawDropStarts{}, rawDropsAtEnd{};
    std::array<std::uint64_t, 4> unavailableStarts{}, unavailableAtEnd{};
    std::uint64_t logBudgetDropsAtEnd = 0;
    std::uint64_t controlStart = 0, controlEnd = 0, controlDrops = 0;
    std::uint64_t eventStartedNs = 0;
    std::uint64_t eventStoppedNs = 0;
    std::uint64_t lastEventSequence = 0;
    std::uint64_t sealedAudioLogs = 0;
    std::uint64_t sealedLogicLogs = 0;
    std::uint64_t eventSourceDrops = 0;
    std::uint64_t logDropBaseline = 0;
    bool eventDrained = false;
    std::uint64_t finalSourceDrops = 0;
    std::array<std::uint64_t, 2> executionStarts{};
    std::array<std::uint64_t, 2> executionEnds{};
    std::array<DropAccum, 2> executionSourceDrops{};

    void Reset() {
        live.store(false, std::memory_order_release);
        queuePtr.store(nullptr, std::memory_order_release);
        generation = 0;
        epoch = 0;
        requestedStreamKindMask.store(0, std::memory_order_relaxed);
        requestedPermissionMask = 0;
        acceptedStreamKindMask = 0;
        acceptedPermissionMask = 0;
        deniedPermissionMask = 0;
        unsupportedPermissionMask = 0;
        requestRawAudio = 0;
        requestSymbols = 0;
        rawAudioDecision = 0;
        symbolsDecision = 0;
        executionFactVersion = 0;
        supportedFactKinds = {};
        baselineAvailability = VN_MONITOR_OBSERVATION_NOT_REQUESTED;
        symbolAvailability = VN_MONITOR_OBSERVATION_NOT_REQUESTED;
        principalHash = 0;
        consumerQueueBytes = 0;
        firstExpectedTimeSeriesSequence = 0;
        firstExpectedStateSequence = 0;
        pollInFlight.store(0, std::memory_order_relaxed);
        resyncRequired.store(false, std::memory_order_relaxed);
        streamTerminated.store(false, std::memory_order_relaxed);
        cut = {};
        identity = {};
        identityStart = identityEnd = 0;
        identityDrops = {};
        baselineDrops = {};
        symbolStart = symbolEnd = 0;
        symbolUnavailableStart = symbolUnavailableEnd = {};
        symbolDrops = {};
        sealRequested = false;
        eventCatalog = false;
        recordingReceiver = false;
        identityInterestReleased = false;
        ringStarts = ringEnds = logStarts = nativeLogDrops = {};
        logFiltered = logLost = logDownstreamEnds = {};
        rawDropStarts = rawDropsAtEnd = {};
        unavailableStarts = unavailableAtEnd = {};
        logBudgetDropsAtEnd = 0;
        controlStart = controlEnd = controlDrops = 0;
        eventStartedNs = 0;
        eventStoppedNs = 0;
        lastEventSequence = 0;
        sealedAudioLogs = 0;
        sealedLogicLogs = 0;
        eventSourceDrops = 0;
        logDropBaseline = 0;
        eventDrained = false;
        finalSourceDrops = 0;
        executionStarts = {};
        executionEnds = {};
        executionSourceDrops = {};
        if (queue) {
            queue->Detach();
        }
    }
};

bool WantsSymbols(const Session& session) {
    return session.executionFactVersion != 0 && session.requestSymbols != 0 &&
        (session.acceptedPermissionMask & VN_MONITOR_PERM_SYMBOLS) != 0 &&
        (session.acceptedStreamKindMask & (VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT)) != 0;
}

std::uint32_t SymbolStreamKind(const Session& session) {
    return (session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0
        ? VN_MONITOR_STREAM_STATE : VN_MONITOR_STREAM_EVENT;
}

void MarkSymbolIncomplete(Session& session) {
    session.symbolAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
    if ((session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0) {
        session.baselineAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
        if (session.cut.settled && session.cut.result == VN_MONITOR_STATE_CUT_COMPLETE)
            session.cut.result = VN_MONITOR_STATE_CUT_TRUNCATED;
    }
}

struct RetiredSession {
    std::uint64_t generation = 0;
    std::uint64_t consumerQueueBytes = 0;
    std::atomic<std::uint32_t> pollInFlight{0};
    std::unique_ptr<ConsumerQueue> queue;
};

struct ProcessOwner {
    std::mutex mutex;
    Engine* engine = nullptr;
    std::thread::id logicThread;
    std::uint64_t nextGeneration = 1;
    std::uint64_t nextEpoch = 1;
    std::array<Session, VN_MONITOR_HARD_SESSIONS_PER_ENGINE> sessions{};
    std::vector<std::unique_ptr<RetiredSession>> retired;
    std::atomic<bool> pollStall{false};
    std::atomic<std::uint32_t> pollEntered{0};
    MonitorProducerRing producerRing;
    MonitorProducerRing logicRing;
    std::array<MonitorExecutionProducer, 2> executionProducers;
    MonitorProducerRing identityRing;
    MonitorIdentityProducer identityProducer;
    MonitorIdentityMirror identityMirror;
    MonitorProducerRing symbolRing;
    MonitorHostSymbolProducer symbolProducer;
    MonitorHostSymbolMirror symbolMirror;
    std::atomic<bool> symbolResourcesReady{false};
    std::uint64_t symbolStorageBytes = 0;
    std::uint64_t observationRevision = 1;
    std::uint32_t symbolObservers = 0;
    std::atomic<bool> identityResourcesReady{false};
    std::uint64_t nextIdentitySequence = 1;
    std::uint64_t identityStorageBytes = 0;
    std::uint32_t identityMutationDepth = 0;
    std::uint32_t identityObservers = 0;
    std::uint32_t pendingIdentitySeals = 0;
    std::uint64_t identityRefreshes = 0;
    std::int32_t dropIdentityRowForTests = -1;
    std::vector<std::uint8_t> drainScratch{};
    std::vector<std::uint8_t> audioPackScratch{};
    std::vector<float> interleaveScratch{};
    bool captureResourcesReady = false;
    bool failCaptureResourcesForTests = false;
    bool drainPausedForTests = false;
    AtomicDropAccum producerDrop{};
    std::array<std::unique_ptr<ConsumerQueue>, kInternalConsumerCount> internals{};
    std::atomic<std::uint32_t> internalEnabledMask{0};
    std::atomic<std::uint32_t> observerCount{0};
    std::atomic<std::uint32_t> eventObservers{0};
    std::atomic<std::uint32_t> executionObservers{0};
    std::array<std::atomic<std::uint64_t>, 2> ringProjectionCompleted{}, logProjectionCompleted{};
    std::array<std::uint64_t, 2> pendingRingTickets{};
    std::array<std::uint64_t, 2> logTranscriptLast{};
    std::uint64_t controlPublished = 0;
    std::array<std::uint64_t, 4> logicUnavailable{};
    std::atomic<bool> recorderLogicPending{false};
    std::atomic<std::uint64_t> lastEventFanoutSequence{0};
    struct Recorder {
        std::uint32_t state = VN_MONITOR_RECORDING_STATE_STOPPED;
        std::uint32_t errorReason = 0;
        std::uint32_t recoveryNeeded = 0;
        std::uint64_t fileBytes = 0;
        std::uint64_t lastCommittedChunk = 0;
        std::uint64_t nextChunk = 1;
        std::uint64_t fileByteBudget = 0;
        std::uint64_t unflushedBytes = 0;
        std::uint64_t queueEpoch = 0;
        std::vector<std::uint8_t> packBuffer;
        std::filesystem::path path;
        std::ofstream file;
        std::mutex mutex;
        bool typed = false;
        bool forceIncomplete = false;
        bool identityCleanupPending = false;
        std::uint32_t stopReason = 0;
        VnMonitorSessionHandle receiver = 0;
        RecordingDescriptor descriptor{};
        RecordingEnd end{};
        MonitorRecordingVerifier verifier;
        std::vector<std::uint8_t> frameBuffer;
        bool failNextWriteForTests = false;
        bool failNextOptionalWriteForTests = false;
        bool failOptionalRestoreForTests = false;
        void (*beforeFailureCleanupForTests)(void*) = nullptr;
        void* failureCleanupContextForTests = nullptr;
        void (*afterReceiverOpenForTests)(void*) = nullptr;
        void* receiverOpenContextForTests = nullptr;

        void ResetStopped() {
            state = VN_MONITOR_RECORDING_STATE_STOPPED;
            errorReason = 0;
            recoveryNeeded = 0;
            fileBytes = 0;
            lastCommittedChunk = 0;
            nextChunk = 1;
            fileByteBudget = 0;
            unflushedBytes = 0;
            queueEpoch = 0;
            typed = forceIncomplete = identityCleanupPending = false;
            stopReason = 0;
            receiver = 0;
            descriptor = {};
            end = {};
            verifier = {};
            path.clear();
            failNextWriteForTests = false;
            beforeFailureCleanupForTests = afterReceiverOpenForTests = nullptr;
            failureCleanupContextForTests = receiverOpenContextForTests = nullptr;
            if (file.is_open()) {
                file.close();
            }
        }
    } recorder{};
    struct Endpoint {
        std::uint32_t profile = VN_MONITOR_SECURITY_DISABLED;
        std::uint64_t epoch = 0;
        std::uint64_t nextEpoch = 1;
        std::uint64_t localAclSidHash = 0;
        std::uint32_t grantedPermissionMask = 0;
        std::uintptr_t boundHandle = 0;
        std::uintptr_t connectEvent = 0;
#if defined(_WIN32)
        OVERLAPPED connectOverlapped{};
        bool connectIoPending = false;
#endif
        bool rejectsRemoteClients = false;
        bool Enabled() const { return profile != VN_MONITOR_SECURITY_DISABLED; }
        bool Connected() const { return epoch != 0; }
        std::string Name() const {
            if (!Enabled()) {
                return {};
            }
            const std::uint64_t stamp = epoch != 0 ? epoch : nextEpoch;
            return "\\\\.\\pipe\\HarkenMonitor." +
                   std::to_string(static_cast<unsigned long long>(stamp));
        }
    } endpoint{};
    std::thread drainThread;
    std::atomic<bool> drainStop{true};
    std::atomic<bool> drainRunning{false};
    std::atomic<bool> drainHold{false};
    std::atomic<std::uint32_t> drainEntered{0};
    std::atomic<bool> fanoutHold{false};
    std::atomic<std::uint32_t> fanoutEntered{0};
    std::atomic<std::uint64_t> publishedCount{0};
    std::atomic<std::uint64_t> fanoutCompleted{0};
    std::atomic<std::uint64_t> lastLogicFanoutSequence{0};
    std::atomic<std::uint64_t> currentWorldGeneration{0};
    std::array<std::uint64_t, 8> lateWorldGapSeen{};
    std::uint32_t lateWorldGapNext = 0;
    std::atomic<std::uint64_t> drainWakeGeneration{0};
#if defined(__APPLE__)
    dispatch_semaphore_t drainWakeSemaphore = dispatch_semaphore_create(0);
    std::atomic<bool> drainWakePending{false};

    ~ProcessOwner() {
        if (drainWakeSemaphore != nullptr) {
            dispatch_release(drainWakeSemaphore);
        }
    }
#endif
    std::atomic<std::uint64_t> nextTimeSeriesSequence{1};
    struct LifecycleDedupeKey {
        std::uint64_t instanceHandle = 0;
        std::uint64_t instanceGeneration = 0;
        std::uint64_t occurrence = 0;
        std::uint8_t phase = 0;
        std::uint8_t used = 0;
    };
    std::array<LifecycleDedupeKey, 64> lifecycleSeen{};
    std::uint32_t lifecycleSeenNext = 0;

    bool LifecycleAlreadySeen(const LifecycleDedupeKey& key) const {
        for (const LifecycleDedupeKey& seen : lifecycleSeen) {
            if (seen.used != 0 && seen.phase == key.phase &&
                seen.instanceHandle == key.instanceHandle &&
                seen.instanceGeneration == key.instanceGeneration &&
                seen.occurrence == key.occurrence) {
                return true;
            }
        }
        return false;
    }

    void RememberLifecycleKey(const LifecycleDedupeKey& key) {
        LifecycleDedupeKey stored = key;
        stored.used = 1;
        lifecycleSeen[lifecycleSeenNext] = stored;
        lifecycleSeenNext =
            (lifecycleSeenNext + 1u) %
            static_cast<std::uint32_t>(lifecycleSeen.size());
    }
    std::uint64_t nextStateSequence = 1;
    std::uint64_t nextDropSourceSequence = 1ull << 32;
    std::atomic<std::uint32_t> publishMutexHits{0};
    bool failInitAfterThread = false;
    bool dropStateCutEnd = false;
    std::atomic<std::uint64_t> leaseExpiryFrame{0};
    LeaseState lease{};
    std::array<PlanSlot, kTotalPlanSlots> planSlots{};
    std::int32_t activeSlot = -1;
    std::int32_t pendingSlot = -1;
    std::uint64_t pendingLeaseGeneration = 0;
    std::uint64_t capturePlanGeneration = 1;
    std::uint64_t previousGeneration = 0;
    std::uint64_t effectiveFromExecuteFrame = 0;
    std::uint64_t tapRegistryGeneration = 1;
    std::atomic<std::uint64_t> measurementSegmentGeneration{1};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> lastAudioLayout{};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> lastAudioRate{};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> lastAudioChannels{};
    bool dropNextActivation = false;
    bool holdPlanAck = false;
    alignas(64) std::atomic<std::uint64_t> executeFrame{0};
    alignas(64) std::atomic<std::int32_t> publishedPending{-1};
    std::atomic<std::uint64_t> pendingReadyFrame{0};
    std::atomic<std::uint32_t> liveTimeSeriesCadenceHz{0};
    std::atomic<std::uint64_t> liveCapturePlanGeneration{1};
    std::atomic<std::uint32_t> liveAudioArmed{0};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> liveTapKind{};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> liveTapSelector{};
    std::array<std::atomic<std::uint64_t>, kLiveTapSlots> liveTapDescriptorId{};
    std::array<std::atomic<std::uint64_t>, kLiveTapSlots> liveTapDescriptorGeneration{};
    std::array<std::atomic<std::uint64_t>, kLiveTapSlots> liveTapOwnerHandle{};
    std::array<std::atomic<std::uint64_t>, kLiveTapSlots> liveTapOwnerGeneration{};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> liveTapSlotId{};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> liveTapFollowBound{};
    std::array<std::atomic<std::uint32_t>, kLiveTapSlots> liveTapAvailability{};
    std::atomic<std::uint32_t> liveTapCount{0};
    std::atomic<std::uint32_t> liveTapKindMask{0};
    alignas(64) std::atomic<std::uint64_t> livePlanSeq{0};

    std::uint32_t LiveSlotsLocked() const {
        std::uint32_t count = 0;
        for (const Session& session : sessions) {
            if (session.live.load(std::memory_order_acquire)) {
                ++count;
            }
        }
        return count;
    }

    std::uint32_t RetiredSlotsLocked() const {
        return static_cast<std::uint32_t>(retired.size());
    }

    std::uint64_t RetiredQueueBytesLocked() const {
        std::uint64_t bytes = 0;
        for (const auto& session : retired) {
            bytes += session->consumerQueueBytes;
        }
        return bytes;
    }

    std::uint64_t LiveQueueBytesLocked() const {
        std::uint64_t bytes = 0;
        for (const Session& session : sessions) {
            if (session.live.load(std::memory_order_acquire)) {
                bytes += session.consumerQueueBytes + session.identity.rows.size() * sizeof(MonitorExecutionFact) +
                    session.identity.symbols.size() * sizeof(MonitorHostSymbolValue);
            }
        }
        return bytes;
    }

    void SweepRetiredLocked() {
        retired.erase(std::remove_if(retired.begin(), retired.end(),
                                     [](const std::unique_ptr<RetiredSession>& session) {
                                         return session->pollInFlight.load(
                                                    std::memory_order_acquire) == 0;
                                     }),
                      retired.end());
    }

    bool WouldExceedLocked(std::uint32_t extraSlots,
                           std::uint64_t extraBytes) {
        SweepRetiredLocked();
        const std::uint32_t slots =
            LiveSlotsLocked() + RetiredSlotsLocked() + extraSlots;
        const std::uint64_t bytes =
            LiveQueueBytesLocked() + RetiredQueueBytesLocked() + identityStorageBytes + symbolStorageBytes + extraBytes;
        return slots > VN_MONITOR_HARD_PROCESS_SESSION_SLOTS ||
               bytes > VN_MONITOR_HARD_PROCESS_QUEUE_BYTES;
    }

    bool InitWouldExceedLocked() {
        return WouldExceedLocked(VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE, 0);
    }

    void RetireIfPollingLocked(Session& session) {
        if (session.pollInFlight.load(std::memory_order_acquire) == 0) {
            if (session.queue) {
                session.queue->Detach();
            }
            return;
        }
        auto item = std::make_unique<RetiredSession>();
        item->generation = session.generation;
        item->consumerQueueBytes = session.consumerQueueBytes;
        item->pollInFlight.store(
            session.pollInFlight.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        item->queue = std::move(session.queue);
        retired.push_back(std::move(item));
    }

    void WakeDrain() {
        drainWakeGeneration.fetch_add(1, std::memory_order_release);
#if defined(__APPLE__)
        if (drainWakeSemaphore != nullptr &&
            !drainWakePending.exchange(true, std::memory_order_acq_rel)) {
            dispatch_semaphore_signal(drainWakeSemaphore);
        }
#else
        drainWakeGeneration.notify_one();
#endif
    }

    void WaitForDrainWake(std::uint64_t generation) {
#if defined(__APPLE__)
        while (drainWakeGeneration.load(std::memory_order_acquire) == generation) {
            dispatch_semaphore_wait(drainWakeSemaphore, DISPATCH_TIME_FOREVER);
            drainWakePending.exchange(false, std::memory_order_acq_rel);
        }
#else
        drainWakeGeneration.wait(generation, std::memory_order_acquire);
#endif
    }

    void ResetPlanLocked() {
        executeFrame.store(0, std::memory_order_relaxed);
        leaseExpiryFrame.store(0, std::memory_order_relaxed);
        lease = LeaseState{};
        for (PlanSlot& slot : planSlots) {
            slot = PlanSlot{};
            slot.plan.structSize = sizeof(VnMonitorCapturePlan);
        }
        activeSlot = -1;
        pendingSlot = -1;
        pendingLeaseGeneration = 0;
        publishedPending.store(-1, std::memory_order_release);
        pendingReadyFrame.store(0, std::memory_order_release);
        capturePlanGeneration = 1;
        previousGeneration = 0;
        effectiveFromExecuteFrame = 0;
        tapRegistryGeneration = 1;
        measurementSegmentGeneration.store(1, std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < kLiveTapSlots; ++i) {
            lastAudioLayout[i].store(0, std::memory_order_relaxed);
            lastAudioRate[i].store(0, std::memory_order_relaxed);
            lastAudioChannels[i].store(0, std::memory_order_relaxed);
        }
        dropNextActivation = false;
        holdPlanAck = false;
        liveTimeSeriesCadenceHz.store(0, std::memory_order_release);
        liveCapturePlanGeneration.store(1, std::memory_order_release);
        liveAudioArmed.store(0, std::memory_order_release);
        liveTapCount.store(0, std::memory_order_release);
        liveTapKindMask.store(0, std::memory_order_release);
        for (std::uint32_t i = 0; i < kLiveTapSlots; ++i) {
            liveTapKind[i].store(0, std::memory_order_relaxed);
            liveTapSelector[i].store(0, std::memory_order_relaxed);
            liveTapDescriptorId[i].store(0, std::memory_order_relaxed);
            liveTapDescriptorGeneration[i].store(0, std::memory_order_relaxed);
            liveTapOwnerHandle[i].store(0, std::memory_order_relaxed);
            liveTapOwnerGeneration[i].store(0, std::memory_order_relaxed);
            liveTapSlotId[i].store(0, std::memory_order_relaxed);
            liveTapFollowBound[i].store(0, std::memory_order_relaxed);
            liveTapAvailability[i].store(0, std::memory_order_relaxed);
        }
        livePlanSeq.store(0, std::memory_order_release);
    }

    void PublishLivePlanLocked() {
        livePlanSeq.fetch_add(1, std::memory_order_release);
        if (activeSlot < 0) {
            liveTimeSeriesCadenceHz.store(0, std::memory_order_release);
            liveCapturePlanGeneration.store(capturePlanGeneration,
                                            std::memory_order_release);
            liveAudioArmed.store(0, std::memory_order_release);
            liveTapCount.store(0, std::memory_order_release);
            liveTapKindMask.store(0, std::memory_order_release);
            livePlanSeq.fetch_add(1, std::memory_order_release);
            return;
        }
        const PlanSlot& slot = planSlots[static_cast<std::size_t>(activeSlot)];
        const bool armed = !slot.empty &&
                           (slot.plan.streamKindMask & VN_MONITOR_STREAM_TIME_SERIES) != 0;
        liveTimeSeriesCadenceHz.store(armed ? slot.plan.cadenceHz : 0,
                                      std::memory_order_release);
        liveCapturePlanGeneration.store(capturePlanGeneration,
                                        std::memory_order_release);
        bool audioArmed = !slot.empty &&
                          (slot.plan.streamKindMask & VN_MONITOR_STREAM_OPTIONAL_AUDIO) !=
                              0 &&
                          slot.storedTapCount > 0;
        if (audioArmed) {
            bool anyLiveTap = false;
            for (std::uint32_t i = 0; i < slot.storedTapCount; ++i) {
                if (slot.taps[i].tapKind != VN_MONITOR_TAP_DEVICE_OUTPUT &&
                    slot.taps[i].availabilityState == 0 &&
                    slot.taps[i].reason == 0) {
                    anyLiveTap = true;
                    break;
                }
            }
            audioArmed = anyLiveTap;
        }
        liveAudioArmed.store(audioArmed ? 1u : 0u, std::memory_order_release);
        const std::uint32_t tapCount =
            slot.storedTapCount < kLiveTapSlots ? slot.storedTapCount : kLiveTapSlots;
        std::uint32_t kindMask = 0;
        for (std::uint32_t i = 0; i < tapCount; ++i) {
            const VnMonitorTapDescriptor& tap = slot.taps[i];
            liveTapKind[i].store(tap.tapKind, std::memory_order_relaxed);
            liveTapSelector[i].store(tap.selectorMode, std::memory_order_relaxed);
            liveTapDescriptorId[i].store(tap.tapDescriptorId,
                                         std::memory_order_relaxed);
            liveTapDescriptorGeneration[i].store(tap.tapDescriptorGeneration,
                                                 std::memory_order_relaxed);
            liveTapOwnerHandle[i].store(tap.ownerInstanceHandle,
                                        std::memory_order_relaxed);
            liveTapOwnerGeneration[i].store(tap.ownerGeneration,
                                            std::memory_order_relaxed);
            liveTapSlotId[i].store(tap.slotId, std::memory_order_relaxed);
            liveTapFollowBound[i].store(0, std::memory_order_relaxed);
            liveTapAvailability[i].store(tap.availabilityState,
                                          std::memory_order_relaxed);
            if (tap.tapKind < 32u) {
                kindMask |= (1u << tap.tapKind);
            }
        }
        liveTapKindMask.store(kindMask, std::memory_order_release);
        liveTapCount.store(tapCount, std::memory_order_release);
        livePlanSeq.fetch_add(1, std::memory_order_release);
    }

    std::uint32_t OccupiedNormalSlotsLocked() const {
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < kNormalPlanSlots; ++i) {
            if (planSlots[i].occupancy != SlotOccupancy::Free) {
                ++count;
            }
        }
        return count;
    }

    std::int32_t FindFreeNormalSlotLocked() const {
        for (std::uint32_t i = 0; i < kNormalPlanSlots; ++i) {
            if (planSlots[i].occupancy == SlotOccupancy::Free) {
                return static_cast<std::int32_t>(i);
            }
        }
        return -1;
    }

    bool PlanIsEmptyLocked() const {
        if (activeSlot < 0) {
            return true;
        }
        return planSlots[static_cast<std::size_t>(activeSlot)].empty;
    }
};

ProcessOwner& Owner() {
    static ProcessOwner owner;
    return owner;
}

VnResult RequireEngineAndLogicThread(bool pollPath);
bool PublishPacked(const std::uint8_t* bytes, std::uint32_t size);
void StopOwnedRecordingLocked(ProcessOwner& owner, VnMonitorSessionHandle handle,
    std::uint64_t lease, std::uint32_t reason);
void CleanupRecordingReceiverLocked(ProcessOwner& owner);
Session* FindLive(ProcessOwner& owner, VnMonitorSessionHandle handle, bool allowRecording);
std::uint32_t ReadPackedU32(const std::uint8_t* bytes, std::size_t offset);
bool IsStateCutBoundRecordKind(std::uint32_t recordKind);

// The same projection controls raw-source accounting and private receiver delivery.
bool TypedRawProjection(const std::uint8_t* bytes, std::uint32_t size) {
    if (!bytes || size < kPackedEnvelopeBytes) return false;
    const auto kind = ReadPackedU32(bytes, 0);
    if (ReadPackedU32(bytes, 12) == VN_MONITOR_STREAM_STATE) return false;
    if (kind == VN_MONITOR_RECORD_EVENT_DETAIL && size < kPackedEnvelopeBytes + 12) return false;
    return kind == VN_MONITOR_RECORD_EVENT_DETAIL ? RecordingKeepsLegacy(kind,
        ReadPackedU32(bytes, kPackedEnvelopeBytes + 4), ReadPackedU32(bytes, kPackedEnvelopeBytes + 8)) : RecordingKeepsLegacy(kind);
}

std::uint32_t RecordingRawStreams(const std::uint8_t* bytes, std::uint32_t size, MonitorProducerOrigin origin = {}) {
    if (!TypedRawProjection(bytes, size) || origin.source >= static_cast<unsigned>(RecordingSource::AudioLog)) return 0;
    // Audio capture also produces synchronous measurement records on the drain thread.
    return ReadPackedU32(bytes, 12) | (ReadPackedU32(bytes, 0) == VN_MONITOR_RECORD_AUDIO_CHUNK
        ? VN_MONITOR_STREAM_TIME_SERIES : 0u);
}
struct LiveTapSnapshot {
    std::uint32_t kind = 0;
    std::uint32_t selector = 0;
    std::uint32_t slotId = 0;
    std::uint32_t followBound = 0;
    std::uint64_t descriptorId = 0;
    std::uint64_t descriptorGeneration = 0;
    std::uint64_t ownerHandle = 0;
    std::uint64_t ownerGeneration = 0;
    std::uint32_t availability = 0;
};

bool SnapLiveAudioPlan(ProcessOwner& owner, std::uint64_t* generation,
                       std::uint64_t* executeFrame, std::uint32_t* tapCount,
                       LiveTapSnapshot* taps);
bool OwnerMatches(std::uint32_t selector, std::uint64_t boundHandle,
                  std::uint64_t boundGeneration, std::uint64_t liveHandle,
                  std::uint64_t liveGeneration);
bool LiveTapKindArmed(std::uint32_t tapKind);
void PublishPackedAudioChunk(ProcessOwner& owner, std::uint8_t* packed,
                             std::uint32_t packedBytes, std::uint64_t generation,
                             std::uint64_t executeFrame, std::uint32_t tapIndex,
                             std::uint64_t tapId, std::uint64_t tapGeneration,
                             std::uint32_t sampleRate, std::uint32_t channels,
                             std::uint32_t layout, std::uint64_t sourceFrameBegin,
                             const float* interleaved, std::uint32_t frames);
bool MaybeFollowSlotRebind(ProcessOwner& owner, std::uint32_t tapIndex,
                           LiveTapSnapshot* tap, std::uint64_t liveHandle,
                           std::uint64_t liveGeneration,
                           std::uint64_t capturePlanGeneration,
                           std::uint64_t executeFrame, std::uint8_t* packed,
                           std::uint32_t packedBytes);
bool MaybeExactInstanceStale(ProcessOwner& owner, std::uint32_t tapIndex,
                             LiveTapSnapshot* tap,
                             std::uint64_t capturePlanGeneration,
                             std::uint64_t executeFrame, std::uint8_t* packed,
                             std::uint32_t packedBytes);

constexpr std::size_t kPackedSourceSequenceOffset = 40;
constexpr std::size_t kPackedDeliverySequenceOffset = 48;
constexpr std::size_t kPackedSessionEpochOffset = 64;
constexpr std::size_t kPackedWorldGenerationOffset = 72;

bool IsStateCutRecordKind(std::uint32_t recordKind) {
    return recordKind == VN_MONITOR_RECORD_STATE_CUT_BEGIN ||
           recordKind == VN_MONITOR_RECORD_STATE_CUT_END;
}

bool IsStateCutBoundRecordKind(std::uint32_t recordKind) {
    return IsStateCutRecordKind(recordKind) ||
           recordKind == VN_MONITOR_RECORD_STATE_UPSERT ||
           recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE;
}

std::uint64_t ReadPackedU64(const std::uint8_t* bytes, std::size_t offset) {
    const std::uint32_t lo =
        static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    const std::uint32_t hi =
        static_cast<std::uint32_t>(bytes[offset + 4]) |
        (static_cast<std::uint32_t>(bytes[offset + 5]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 6]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 7]) << 24);
    return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
}

std::uint32_t ReadPackedU32(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void StampPackedU64(std::uint8_t* bytes, std::size_t offset, std::uint64_t value) {
    WriteU64(bytes + offset, value);
}

std::uint64_t Mix64(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value == 0 ? 1 : value;
}

std::uint64_t HashBytes(const void* bytes, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(bytes);
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return Mix64(hash);
}

std::uint64_t PrincipalHashFor(const Session& session) {
    if (session.principalHash != 0) {
        return session.principalHash;
    }
    return Mix64((session.epoch << 8) ^ session.generation);
}

#if defined(_WIN32)
void CloseBoundPipe(ProcessOwner::Endpoint& endpoint) {
    if (endpoint.boundHandle != 0) {
        HANDLE handle = reinterpret_cast<HANDLE>(endpoint.boundHandle);
        if (endpoint.connectIoPending) {
            CancelIoEx(handle, &endpoint.connectOverlapped);
            DWORD ignored = 0;
            GetOverlappedResult(handle, &endpoint.connectOverlapped, &ignored, TRUE);
            endpoint.connectIoPending = false;
        }
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
        endpoint.boundHandle = 0;
    }
    if (endpoint.connectEvent != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(endpoint.connectEvent));
        endpoint.connectEvent = 0;
    }
    endpoint.connectOverlapped = {};
    endpoint.connectIoPending = false;
    endpoint.rejectsRemoteClients = false;
    endpoint.localAclSidHash = 0;
}

bool CurrentProcessOwnerSid(PSID* outSid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<std::uint8_t> buffer(needed);
    const BOOL ok =
        GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
    CloseHandle(token);
    if (!ok) {
        return false;
    }
    const TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    const DWORD sidBytes = GetLengthSid(user->User.Sid);
    *outSid = LocalAlloc(LPTR, sidBytes);
    if (*outSid == nullptr) {
        return false;
    }
    CopySid(sidBytes, *outSid, user->User.Sid);
    return true;
}

bool BindLocalAclPipe(ProcessOwner::Endpoint& endpoint) {
    CloseBoundPipe(endpoint);
    PSID ownerSid = nullptr;
    if (!CurrentProcessOwnerSid(&ownerSid)) {
        return false;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(ownerSid);
    PACL dacl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &dacl) != ERROR_SUCCESS) {
        LocalFree(ownerSid);
        return false;
    }
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) ||
        !SetSecurityDescriptorOwner(&descriptor, ownerSid, FALSE)) {
        LocalFree(dacl);
        LocalFree(ownerSid);
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    attributes.bInheritHandle = FALSE;
    const std::string name = endpoint.Name();
    HANDLE pipe = CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, &attributes);
    const DWORD created = GetLastError();
    LocalFree(dacl);
    if (pipe == INVALID_HANDLE_VALUE) {
        LocalFree(ownerSid);
        (void)created;
        return false;
    }
    HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (connectEvent == nullptr) {
        CloseHandle(pipe);
        LocalFree(ownerSid);
        return false;
    }
    endpoint.connectOverlapped = {};
    endpoint.connectOverlapped.hEvent = connectEvent;
    const BOOL connected = ConnectNamedPipe(pipe, &endpoint.connectOverlapped);
    const DWORD connectError = connected ? ERROR_PIPE_CONNECTED : GetLastError();
    if (!connected && connectError != ERROR_IO_PENDING &&
        connectError != ERROR_PIPE_CONNECTED) {
        CloseHandle(connectEvent);
        CloseHandle(pipe);
        LocalFree(ownerSid);
        return false;
    }
    if (connectError == ERROR_PIPE_CONNECTED) {
        SetEvent(connectEvent);
        endpoint.connectIoPending = false;
    } else {
        endpoint.connectIoPending = true;
    }
    endpoint.boundHandle = reinterpret_cast<std::uintptr_t>(pipe);
    endpoint.connectEvent = reinterpret_cast<std::uintptr_t>(connectEvent);
    endpoint.rejectsRemoteClients = true;
    const DWORD sidBytes = GetLengthSid(ownerSid);
    endpoint.localAclSidHash = HashBytes(ownerSid, sidBytes);
    LocalFree(ownerSid);
    return true;
}

bool SidHashForProcess(DWORD processId, std::uint64_t* outHash) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        CloseHandle(process);
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<std::uint8_t> buffer(needed);
    const BOOL ok =
        GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
    CloseHandle(token);
    CloseHandle(process);
    if (!ok) {
        return false;
    }
    const TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    *outHash = HashBytes(user->User.Sid, GetLengthSid(user->User.Sid));
    return true;
}

bool AcceptLocalAclClient(ProcessOwner::Endpoint& endpoint) {
    if (endpoint.boundHandle == 0 || endpoint.connectEvent == 0) {
        return false;
    }
    HANDLE pipe = reinterpret_cast<HANDLE>(endpoint.boundHandle);
    HANDLE connectEvent = reinterpret_cast<HANDLE>(endpoint.connectEvent);
    if (WaitForSingleObject(connectEvent, 2000) != WAIT_OBJECT_0) {
        return false;
    }
    if (endpoint.connectIoPending) {
        DWORD ignored = 0;
        if (!GetOverlappedResult(pipe, &endpoint.connectOverlapped, &ignored, FALSE) &&
            GetLastError() != ERROR_PIPE_CONNECTED) {
            return false;
        }
        endpoint.connectIoPending = false;
    }
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || clientPid == 0) {
        return false;
    }
    std::uint64_t clientHash = 0;
    if (!SidHashForProcess(clientPid, &clientHash) ||
        clientHash != endpoint.localAclSidHash) {
        return false;
    }
    return true;
}
#else
void CloseBoundPipe(ProcessOwner::Endpoint&) {}
bool BindLocalAclPipe(ProcessOwner::Endpoint&) { return false; }
bool AcceptLocalAclClient(ProcessOwner::Endpoint&) { return false; }
#endif

bool KnownTapKind(std::uint32_t tapKind) {
    return tapKind >= VN_MONITOR_TAP_VOICE_PRE_CHAIN &&
           tapKind <= VN_MONITOR_TAP_DEVICE_OUTPUT;
}

bool KnownSelectorMode(std::uint32_t selectorMode) {
    return selectorMode == VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE ||
           selectorMode == VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
}

VnResult CopyPlanTaps(const VnMonitorCapturePlan* src, PlanSlot* slot) {
    slot->storedTapCount = 0;
    slot->taps = {};
    if (src->tapCount == 0 && src->taps == nullptr) {
        return VN_OK;
    }
    if (src->tapCount == 0 || src->taps == nullptr) {
        return VN_INVALID_ARG;
    }
    if (src->tapCount > VN_MONITOR_HARD_TAPS_PER_PLAN) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (src->downsample != 0 && src->downsample != 1) {
        return VN_MONITOR_UNSUPPORTED;
    }
    for (std::uint32_t i = 0; i < src->tapCount; ++i) {
        const VnMonitorTapDescriptor& tap = src->taps[i];
        if (!ValidStructSize(tap.structSize, sizeof(VnMonitorTapDescriptor))) {
            slot->taps = {};
            slot->storedTapCount = 0;
            return VN_INVALID_ARG;
        }
        if (!KnownTapKind(tap.tapKind) || !KnownSelectorMode(tap.selectorMode)) {
            slot->taps = {};
            slot->storedTapCount = 0;
            return VN_MONITOR_UNSUPPORTED;
        }
        slot->taps[i] = tap;
        if (slot->taps[i].tapDescriptorId == 0) {
            slot->taps[i].tapDescriptorId = tap.tapKind;
        }
        if (slot->taps[i].tapDescriptorGeneration == 0) {
            slot->taps[i].tapDescriptorGeneration = 1;
        }
        if (slot->taps[i].tapKind == VN_MONITOR_TAP_DEVICE_OUTPUT) {
            slot->taps[i].ownerKind = VN_MONITOR_ENTITY_NONE;
            slot->taps[i].availabilityState = 1;
            slot->taps[i].reason = VN_MONITOR_UNSUPPORTED;
        } else if (slot->taps[i].tapKind == VN_MONITOR_TAP_VOICE_PRE_CHAIN ||
                   slot->taps[i].tapKind == VN_MONITOR_TAP_VOICE_POST_CHAIN) {
            slot->taps[i].ownerKind = VN_MONITOR_ENTITY_VOICE;
        } else if (slot->taps[i].tapKind == VN_MONITOR_TAP_EFFECT_SLOT_INPUT ||
                   slot->taps[i].tapKind == VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT) {
            slot->taps[i].ownerKind = VN_MONITOR_ENTITY_EFFECT;
        } else if (slot->taps[i].tapKind == VN_MONITOR_TAP_BUS_POST_EFFECTS ||
                   slot->taps[i].tapKind == VN_MONITOR_TAP_BUS_PRE_FADER ||
                   slot->taps[i].tapKind == VN_MONITOR_TAP_BUS_POST_FADER ||
                   slot->taps[i].tapKind == VN_MONITOR_TAP_MASTER_OUTPUT) {
            slot->taps[i].ownerKind = VN_MONITOR_ENTITY_BUS;
        }
        if (slot->taps[i].selectorMode == VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE &&
            slot->taps[i].tapKind != VN_MONITOR_TAP_DEVICE_OUTPUT &&
            slot->taps[i].tapKind != VN_MONITOR_TAP_MASTER_OUTPUT &&
            ((slot->taps[i].ownerKind == VN_MONITOR_ENTITY_BUS
                  ? slot->taps[i].ownerStableId == VnObjectId{}
                  : slot->taps[i].ownerInstanceHandle == 0) ||
             slot->taps[i].ownerGeneration == 0)) {
            slot->taps = {};
            slot->storedTapCount = 0;
            return VN_INVALID_ARG;
        }
        ProcessOwner& owner = Owner();
        if (slot->taps[i].tapKind == VN_MONITOR_TAP_MASTER_OUTPUT) {
            slot->taps[i].ownerKind = VN_MONITOR_ENTITY_BUS;
            slot->taps[i].ownerStableId = VN_BUS_MASTER;
            slot->taps[i].ownerInstanceHandle = 1;
            std::uint64_t liveGeneration = 0;
            if (owner.engine != nullptr) {
                const std::uint32_t busIndex =
                    owner.engine->buses.FindIndex(VN_BUS_MASTER);
                if (busIndex != kInvalidBusIndex) {
                    liveGeneration = owner.engine->buses.GenerationAt(busIndex);
                    if (slot->taps[i].ownerGeneration == 0) {
                        slot->taps[i].ownerGeneration = liveGeneration;
                    }
                }
            }
            if (slot->taps[i].selectorMode ==
                    VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE &&
                slot->taps[i].ownerGeneration != 0 && liveGeneration != 0 &&
                slot->taps[i].ownerGeneration != liveGeneration) {
                slot->taps = {};
                slot->storedTapCount = 0;
                return VN_MONITOR_STALE_TAP;
            }
        } else if (slot->taps[i].selectorMode ==
                       VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE &&
                   owner.engine != nullptr) {
            bool stale = false;
            if (slot->taps[i].tapKind == VN_MONITOR_TAP_VOICE_PRE_CHAIN ||
                slot->taps[i].tapKind == VN_MONITOR_TAP_VOICE_POST_CHAIN) {
                const VoiceLedger::Resolved resolved =
                    owner.engine->ledger.Classify(slot->taps[i].ownerInstanceHandle);
                stale = resolved.kind != VoiceLedger::HandleKind::Live ||
                        resolved.renderHandle == 0;
                if (!stale) {
                    const std::uint32_t publicGeneration = static_cast<std::uint32_t>(
                        slot->taps[i].ownerInstanceHandle >> 32);
                    stale = publicGeneration != 0 &&
                            publicGeneration !=
                                static_cast<std::uint32_t>(
                                    slot->taps[i].ownerGeneration);
                }
                if (!stale) {
                    stale = owner.engine->voices.CheckLive(resolved.renderHandle) !=
                            VN_OK;
                }
            } else if (slot->taps[i].tapKind == VN_MONITOR_TAP_BUS_POST_EFFECTS ||
                       slot->taps[i].tapKind == VN_MONITOR_TAP_BUS_PRE_FADER ||
                       slot->taps[i].tapKind == VN_MONITOR_TAP_BUS_POST_FADER) {
                const std::uint32_t busIndex = owner.engine->buses.FindIndex(
                    slot->taps[i].ownerStableId);
                stale = busIndex == kInvalidBusIndex ||
                        owner.engine->buses.GenerationAt(busIndex) !=
                            slot->taps[i].ownerGeneration;
                if (!stale) {
                    slot->taps[i].ownerInstanceHandle = busIndex + 1u;
                }
            } else if (slot->taps[i].tapKind == VN_MONITOR_TAP_EFFECT_SLOT_INPUT ||
                       slot->taps[i].tapKind == VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT) {
                const std::uint32_t busIndex =
                    static_cast<std::uint32_t>(slot->taps[i].ownerInstanceHandle >> 32);
                const std::uint32_t packedSlot = static_cast<std::uint32_t>(
                    slot->taps[i].ownerInstanceHandle & 0xffffffffu);
                const std::uint32_t effectSlot =
                    packedSlot == 0 ? 0u : packedSlot - 1u;
                const BusTree::Bus* bus = owner.engine->buses.BusState(busIndex);
                stale = bus == nullptr || packedSlot == 0 ||
                        effectSlot >= kMaxFxSlotsPerBus ||
                        bus->fxSlotEffectRef[effectSlot].type == kEffectTypeNone ||
                        bus->fxSlotEffectRef[effectSlot].generation !=
                            slot->taps[i].ownerGeneration ||
                        PackedMonitorEffectHandle(busIndex, effectSlot) !=
                            slot->taps[i].ownerInstanceHandle;
            }
            if (stale) {
                slot->taps = {};
                slot->storedTapCount = 0;
                return VN_MONITOR_STALE_TAP;
            }
        }
    }
    slot->storedTapCount = src->tapCount;
    return VN_OK;
}

VnResult CopyPlanMeasurements(const VnMonitorCapturePlan* src, PlanSlot* slot) {
    slot->storedMeasurementCount = 0;
    slot->measurements = {};
    if (src->measurementCount == 0 && src->measurements == nullptr) {
        return VN_OK;
    }
    if (src->measurementCount == 0 || src->measurements == nullptr) {
        return VN_INVALID_ARG;
    }
    if (src->measurementCount > VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (slot->storedTapCount == 0) {
        return VN_INVALID_ARG;
    }
    for (std::uint32_t i = 0; i < src->measurementCount; ++i) {
        const VnMonitorMeasurementRequest& request = src->measurements[i];
        if (!ValidStructSize(request.structSize,
                             sizeof(VnMonitorMeasurementRequest))) {
            slot->measurements = {};
            slot->storedMeasurementCount = 0;
            return VN_INVALID_ARG;
        }
        if (!ReservedZero(request.reserved0) || !ReservedZero(request.reserved1)) {
            slot->measurements = {};
            slot->storedMeasurementCount = 0;
            return VN_INVALID_ARG;
        }
        if (request.tapIndex >= slot->storedTapCount) {
            slot->measurements = {};
            slot->storedMeasurementCount = 0;
            return VN_INVALID_ARG;
        }
        const VnResult validated =
            ValidateDerivedRequest(request, slot->taps[request.tapIndex]);
        if (validated != VN_OK) {
            slot->measurements = {};
            slot->storedMeasurementCount = 0;
            return validated;
        }
        if (request.kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY &&
            !core::LoudnessSampleRateSupported(
                Owner().engine->config.mixRate.SampleRate())) {
            slot->measurements = {};
            slot->storedMeasurementCount = 0;
            return VN_MONITOR_UNSUPPORTED;
        }
        slot->measurements[i] = request;
    }
    slot->storedMeasurementCount = src->measurementCount;
    return VN_OK;
}

VnResult CopyPlan(const VnMonitorCapturePlan* src, VnMonitorCapturePlan* dst,
                  PlanSlot* slot) {
    if (src == nullptr || dst == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(src->structSize, sizeof(VnMonitorCapturePlan))) {
        return VN_INVALID_ARG;
    }
    if (!ReservedZero(src->reserved0) || !ReservedZero(src->reserved1)) {
        return VN_INVALID_ARG;
    }
    if (slot == nullptr) {
        PlanSlot probe{};
        const VnResult taps = CopyPlanTaps(src, &probe);
        if (taps != VN_OK) {
            return taps;
        }
        const VnResult measurements = CopyPlanMeasurements(src, &probe);
        if (measurements != VN_OK) {
            return measurements;
        }
        *dst = *src;
        dst->measurements =
            probe.storedMeasurementCount == 0 ? nullptr : src->measurements;
        dst->measurementCount = probe.storedMeasurementCount;
        return VN_OK;
    }
    const VnResult taps = CopyPlanTaps(src, slot);
    if (taps != VN_OK) {
        return taps;
    }
    const VnResult measurements = CopyPlanMeasurements(src, slot);
    if (measurements != VN_OK) {
        slot->taps = {};
        slot->storedTapCount = 0;
        return measurements;
    }
    *dst = *src;
    dst->tapCount = slot->storedTapCount;
    dst->taps = slot->storedTapCount == 0 ? nullptr : slot->taps.data();
    dst->measurementCount = slot->storedMeasurementCount;
    dst->measurements = slot->storedMeasurementCount == 0
                            ? nullptr
                            : slot->measurements.data();
    return VN_OK;
}

bool PlanLooksEmpty(const VnMonitorCapturePlan& plan) {
    return plan.streamKindMask == 0 && plan.cadenceHz == 0 &&
           plan.bytesPerSecondBudget == 0 && plan.sessionByteBudget == 0 &&
           plan.maxDurationExecuteFrames == 0 && plan.tapCount == 0;
}

void FillEmptyPlan(VnMonitorCapturePlan* plan) {
    std::memset(plan, 0, sizeof(*plan));
    plan->structSize = sizeof(*plan);
}

void ReleaseFailedSlot(PlanSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    slot->derivedState.reset();
    slot->occupancy = SlotOccupancy::Free;
    slot->empty = true;
    slot->quarantined = false;
    slot->storedTapCount = 0;
    slot->storedMeasurementCount = 0;
    slot->taps = {};
    slot->measurements = {};
    FillEmptyPlan(&slot->plan);
}

std::uint64_t PackedBytesPerQuantumForMeasurementRequest(
    const VnMonitorMeasurementRequest& request) {
    switch (request.kind) {
    case VN_MONITOR_MEASURE_WAVEFORM: {
        const std::uint32_t bucketFrames =
            request.bucketFrames != 0 ? request.bucketFrames : 1u;
        // Floor, not ceil: hop/bucket wider than one quantum emits across
        // quanta. packed * frames / span keeps 4096/2048 from truncating to 0.
        return static_cast<std::uint64_t>(PackedWaveformBucketBytes()) *
               static_cast<std::uint64_t>(VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES) /
               bucketFrames;
    }
    case VN_MONITOR_MEASURE_SCOPE:
        return PackedScopeResultBytes();
    case VN_MONITOR_MEASURE_SPECTRUM: {
        const std::uint32_t fftSize = request.fftSize != 0 ? request.fftSize : 512u;
        const std::uint32_t hop = core::SpectrumHop(fftSize);
        const std::uint32_t span = hop != 0 ? hop : 1u;
        return static_cast<std::uint64_t>(PackedSpectrumBytes(fftSize / 2u + 1u)) *
               static_cast<std::uint64_t>(VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES) /
               span;
    }
    case VN_MONITOR_MEASURE_GAIN_REDUCTION:
        return PackedGainReductionBytes();
    default:
        return PackedMeterBytes();
    }
}

std::uint64_t PredictedPlanByteRate(const VnMonitorCapturePlan& plan) {
    // Time-series-only plans stay at predicted 0 so a 1024 user budget is not
    // mis-hit. Derived cost is counted only with OPTIONAL_AUDIO, because drain
    // only emits measurements from AUDIO_CHUNK.
    std::uint64_t predicted = 0;
    if ((plan.streamKindMask & VN_MONITOR_STREAM_OPTIONAL_AUDIO) == 0) {
        return predicted;
    }
    const std::uint64_t taps =
        plan.tapCount == 0 ? 1ull : static_cast<std::uint64_t>(plan.tapCount);
    // cadenceHz is the time-series clock. Audio publishes at the default
    // quantum (100 Hz) unless a later pack exposes a real device rate.
    constexpr std::uint64_t kAudioPublishHz = 100ull;
    predicted += static_cast<std::uint64_t>(PackedAudioChunkBytes(
                     VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES,
                     kPackedAudioMaxChannels)) *
                 taps * kAudioPublishHz;
    if (plan.measurementCount == 0 || plan.measurements == nullptr) {
        return predicted;
    }
    for (std::uint32_t i = 0; i < plan.measurementCount; ++i) {
        predicted += PackedBytesPerQuantumForMeasurementRequest(plan.measurements[i]) *
                     taps * kAudioPublishHz;
    }
    return predicted;
}

void ClearEffectivePointers(VnMonitorEffectiveCapturePlan* out) {
    out->accepted.taps = nullptr;
    out->accepted.measurements = nullptr;
}

void WriteActiveLocked(const ProcessOwner& owner,
                       VnMonitorEffectiveCapturePlan* out) {
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->activationState = VN_MONITOR_PLAN_ACTIVE;
    out->capturePlanGeneration = owner.capturePlanGeneration;
    out->previousGeneration = owner.previousGeneration;
    out->effectiveFromExecuteFrame = owner.effectiveFromExecuteFrame;
    if (owner.activeSlot >= 0) {
        out->accepted =
            owner.planSlots[static_cast<std::size_t>(owner.activeSlot)].plan;
    } else {
        FillEmptyPlan(&out->accepted);
    }
    ClearEffectivePointers(out);
}

void WritePendingLocked(const ProcessOwner& owner,
                        VnMonitorEffectiveCapturePlan* out) {
    const PlanSlot& pending =
        owner.planSlots[static_cast<std::size_t>(owner.pendingSlot)];
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->activationState = VN_MONITOR_PLAN_PENDING;
    out->capturePlanGeneration = owner.capturePlanGeneration + 1;
    out->previousGeneration = owner.capturePlanGeneration;
    out->effectiveFromExecuteFrame =
        owner.pendingReadyFrame.load(std::memory_order_relaxed);
    out->accepted = pending.plan;
    ClearEffectivePointers(out);
}

void WriteEffective(const ProcessOwner& owner, VnMonitorEffectiveCapturePlan* out) {
    if (owner.pendingSlot >= 0) {
        WritePendingLocked(owner, out);
    } else {
        WriteActiveLocked(owner, out);
    }
}

void WriteEffectiveGeneration(const ProcessOwner& owner, std::uint64_t generation,
                              VnMonitorEffectiveCapturePlan* out) {
    if (generation == 0) {
        WriteEffective(owner, out);
        return;
    }
    if (owner.pendingSlot >= 0 && generation == owner.capturePlanGeneration + 1) {
        WritePendingLocked(owner, out);
        return;
    }
    WriteActiveLocked(owner, out);
}

bool GenerationMatchesLocked(const ProcessOwner& owner, std::uint64_t generation) {
    if (generation == 0) {
        return true;
    }
    if (owner.pendingSlot >= 0 && generation == owner.capturePlanGeneration + 1) {
        return true;
    }
    return generation == owner.capturePlanGeneration;
}

void FillLeaseStatus(const ProcessOwner& owner, VnMonitorCaptureControlStatus* out) {
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->leaseGeneration = owner.lease.generation;
    out->principalHash = owner.lease.principalHash;
    out->permissions = owner.lease.permissions;
    out->held = owner.lease.held ? 1u : 0u;
    out->expiryExecuteFrame = owner.lease.expiryExecuteFrame;
    out->lastChangeReason = owner.lease.lastChangeReason;
}

bool ExpireLeaseIfDueLocked(ProcessOwner& owner) {
    if (!owner.lease.held || owner.lease.expiryExecuteFrame == 0) {
        return false;
    }
    if (owner.executeFrame.load(std::memory_order_relaxed) <
        owner.lease.expiryExecuteFrame) {
        return false;
    }
    StopOwnedRecordingLocked(owner, owner.lease.holder, owner.lease.generation, kLeaseReasonExpiry);
    owner.lease.held = false;
    owner.lease.holder = 0;
    owner.lease.principalHash = 0;
    owner.lease.permissions = 0;
    owner.lease.generation += 1;
    owner.lease.lastChangeReason = kLeaseReasonExpiry;
    return true;
}

void DisconnectHolderLocked(ProcessOwner& owner, VnMonitorSessionHandle handle) {
    if (!owner.lease.held || owner.lease.holder != handle) {
        return;
    }
    StopOwnedRecordingLocked(owner, owner.lease.holder, owner.lease.generation, kLeaseReasonDisconnect);
    owner.lease.held = false;
    owner.lease.holder = 0;
    owner.lease.principalHash = 0;
    owner.lease.permissions = 0;
    owner.lease.generation += 1;
    owner.lease.lastChangeReason = kLeaseReasonDisconnect;
}

void DisconnectRemoteControlLocked(ProcessOwner& owner) {
    if (!owner.lease.held) {
        return;
    }
    StopOwnedRecordingLocked(owner, owner.lease.holder, owner.lease.generation, kLeaseReasonDisconnect);
    owner.lease.held = false;
    owner.lease.holder = 0;
    owner.lease.principalHash = 0;
    owner.lease.permissions = 0;
    owner.lease.generation += 1;
    owner.lease.lastChangeReason = kLeaseReasonDisconnect;
}

void RetireLingeringLocked(ProcessOwner& owner) {
    for (std::uint32_t i = 0; i < kTotalPlanSlots; ++i) {
        PlanSlot& slot = owner.planSlots[i];
        if (slot.occupancy != SlotOccupancy::Lingering) {
            continue;
        }
        if (owner.holdPlanAck) {
            continue;
        }
        if (owner.activeSlot == static_cast<std::int32_t>(i)) {
            continue;
        }
        slot.derivedState.reset();
        slot.occupancy = SlotOccupancy::Free;
        slot.empty = true;
        FillEmptyPlan(&slot.plan);
        slot.quarantined = false;
    }
}

void ActivatePendingLocked(ProcessOwner& owner) {
    if (owner.pendingSlot < 0) {
        return;
    }
    const std::int32_t next = owner.pendingSlot;
    if (owner.activeSlot >= 0 && owner.activeSlot != next) {
        PlanSlot& old = owner.planSlots[static_cast<std::size_t>(owner.activeSlot)];
        if (old.quarantined) {
            old.occupancy = SlotOccupancy::Quarantine;
        } else if (owner.holdPlanAck) {
            old.occupancy = SlotOccupancy::Lingering;
        } else {
            old.derivedState.reset();
            old.occupancy = SlotOccupancy::Free;
            old.empty = true;
            FillEmptyPlan(&old.plan);
        }
    }
    PlanSlot& slot = owner.planSlots[static_cast<std::size_t>(next)];
    slot.occupancy = SlotOccupancy::Active;
    owner.previousGeneration = owner.capturePlanGeneration;
    owner.capturePlanGeneration += 1;
    if (!slot.empty && !PlanLooksEmpty(slot.plan)) {
        owner.measurementSegmentGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    owner.effectiveFromExecuteFrame =
        owner.executeFrame.load(std::memory_order_relaxed);
    owner.activeSlot = next;
    owner.pendingSlot = -1;
    owner.pendingLeaseGeneration = 0;
    owner.publishedPending.store(-1, std::memory_order_release);
    owner.pendingReadyFrame.store(0, std::memory_order_release);
    owner.PublishLivePlanLocked();
}

std::uint32_t PrepareActivationLocked(ProcessOwner& owner, std::uint8_t* packed,
                                      std::uint32_t capacity,
                                      std::uint32_t reason) {
    if (owner.dropNextActivation) {
        owner.dropNextActivation = false;
        const std::uint64_t sourceSequence =
            owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
        owner.producerDrop.Note(sourceSequence, PackedActivationBytes(),
                                VN_MONITOR_EVENT_DROP);
        return 0;
    }
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    return PackCapturePlanActivated(packed, capacity, sourceSequence,
                                    owner.capturePlanGeneration,
                                    owner.effectiveFromExecuteFrame, reason);
}

VnResult CopyValidatedPlan(const VnMonitorCapturePlan* plan,
                           VnMonitorCapturePlan* out, PlanSlot* slot) {
    if (plan == nullptr || out == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult copied = CopyPlan(plan, out, slot);
    if (copied != VN_OK) {
        if (slot != nullptr) {
            ReleaseFailedSlot(slot);
        }
        return copied;
    }
    if (out->bytesPerSecondBudget > VN_MONITOR_HARD_CAPTURE_BYTE_RATE ||
        out->sessionByteBudget > VN_MONITOR_HARD_PROCESS_QUEUE_BYTES) {
        if (slot != nullptr) {
            ReleaseFailedSlot(slot);
        }
        return VN_MONITOR_BUDGET_EXCEEDED;
    }
    const std::uint64_t predicted = PredictedPlanByteRate(*out);
    if (predicted > VN_MONITOR_HARD_CAPTURE_BYTE_RATE) {
        if (slot != nullptr) {
            ReleaseFailedSlot(slot);
        }
        return VN_MONITOR_BUDGET_EXCEEDED;
    }
    if (out->bytesPerSecondBudget != 0 && out->bytesPerSecondBudget < predicted) {
        if (slot != nullptr) {
            ReleaseFailedSlot(slot);
        }
        return VN_MONITOR_BUDGET_EXCEEDED;
    }
    return VN_OK;
}

bool IsCurrentHolderLocked(const ProcessOwner& owner, const Session& session,
                           VnMonitorSessionHandle handle,
                           std::uint64_t leaseGeneration) {
    return owner.lease.held && owner.lease.holder == handle &&
           owner.lease.generation == leaseGeneration &&
           owner.lease.principalHash == PrincipalHashFor(session);
}

bool StampDelivery(std::uint8_t* bytes, std::uint32_t size, std::uint64_t delivery) {
    if (bytes == nullptr || size < kPackedEnvelopeBytes) {
        return false;
    }
    WriteU64(bytes + kPackedDeliverySequenceOffset, delivery);
    return true;
}

bool EnqueueRecord(ConsumerQueue& queue, const std::uint8_t* bytes,
                   std::uint32_t size, std::uint64_t sourceSequence,
                   std::uint32_t recordKind, std::uint32_t dropStage,
                   bool countDropOnFull, std::uint64_t expectedEpoch = 0,
                   std::uint64_t expectedGeneration = 0, bool deferSymbol = false,
                   MonitorProducerOrigin origin = {}) {
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (expectedEpoch != 0 && queue.epoch != expectedEpoch) {
        return false;
    }
    if (expectedGeneration != 0 && queue.generation != expectedGeneration) {
        return false;
    }
    if (queue.budgetBytes > 0 &&
        queue.usedBytes + size > queue.budgetBytes) {
        if (countDropOnFull) {
            const auto stream = ReadPackedU64(bytes, 24);
            unsigned index = 0;
            if (recordKind == VN_MONITOR_RECORD_EXECUTION_FACT)
                index = stream == kExecutionLogicStream ? 1u : (stream == kExecutionAudioStream ? 2u : 0u);
            else if (IsIdentityRecord(recordKind))
                index = stream == kIdentityDeltaStream ? 3u : (stream == kIdentityBaselineStream ? 4u :
                    (stream == kHostSymbolStream ? 5u : 0u));
            auto& drop = queue.drops[index];
            drop.stage = dropStage;
            drop.Note(sourceSequence, size, VN_MONITOR_EVENT_DROP);
            ++queue.droppedRecords;
            ++queue.cumulativeDrops[index];
        }
        return false;
    }
    PackedRecord record;
    record.bytes.assign(bytes, bytes + size);
    record.sourceSequence = sourceSequence;
    record.recordKind = recordKind;
    record.origin = origin;
    const std::uint64_t delivery = deferSymbol ? 0 : queue.nextDelivery++;
    StampDelivery(record.bytes.data(), size, delivery);
    if (recordKind == VN_MONITOR_RECORD_EVENT_DETAIL || queue.recordingReceiver ||
        (recordKind >= VN_MONITOR_RECORD_EXECUTION_FACT && recordKind <= VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL)) {
        WriteU64(record.bytes.data() + kPackedSessionEpochOffset, queue.epoch);
    }
    queue.usedBytes += size;
    if (deferSymbol) queue.deferredSymbols.push_back(std::move(record));
    else queue.records.push_back(std::move(record));
    return true;
}

bool FlushDropSummary(ProcessOwner& owner, ConsumerQueue& queue,
                      std::uint32_t dropStage, std::uint64_t expectedEpoch = 0,
                      std::uint64_t expectedGeneration = 0, bool deferSymbols = false) {
    for (unsigned index = 0; index != queue.drops.size(); ++index) {
        VnMonitorDropSummary snapshot{};
        bool deferBucket = false;
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (expectedEpoch != 0 && queue.epoch != expectedEpoch) {
                return false;
            }
            if (expectedGeneration != 0 && queue.generation != expectedGeneration) {
                return false;
            }
            const auto& drop = queue.drops[index];
            if (drop.recordCount == 0) continue;
            snapshot.structSize = sizeof(snapshot);
            snapshot.stage = drop.stage ? drop.stage : dropStage;
            snapshot.streamId = drop.streamId;
            snapshot.firstSourceSequence = drop.firstSourceSequence;
            snapshot.lastSourceSequence = drop.lastSourceSequence;
            snapshot.recordCount = drop.recordCount;
            snapshot.byteCount = drop.byteCount;
            snapshot.reason = drop.reason;
            deferBucket = snapshot.streamId == kHostSymbolStream &&
                (deferSymbols || queue.symbolBaselineActive || !queue.deferredSymbols.empty());
        }
        std::uint8_t packed[kScratchBytes];
        const std::uint64_t sourceSequence = owner.nextDropSourceSequence++;
        const std::uint32_t size =
            PackDropSummaryRecord(packed, sizeof(packed), snapshot, sourceSequence);
        if (size == 0) {
            return false;
        }
        if (snapshot.streamId == kIdentityDeltaStream || snapshot.streamId == kIdentityBaselineStream)
            WriteU32(packed + 12, VN_MONITOR_STREAM_STATE);
        if (snapshot.streamId == kHostSymbolStream) WriteU32(packed + 12, queue.symbolStreamKind);
        if (EnqueueRecord(queue, packed, size, sourceSequence,
                          VN_MONITOR_RECORD_DROP_SUMMARY, dropStage, false,
                          expectedEpoch, expectedGeneration, deferBucket)) {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if ((expectedEpoch == 0 || queue.epoch == expectedEpoch) &&
                (expectedGeneration == 0 || queue.generation == expectedGeneration)) {
                queue.drops[index].Clear();
            }
        } else {
            return false;
        }
    }
    return true;
}

std::uint64_t PackedStateCutId(const std::uint8_t* bytes, std::uint32_t size) {
    if (bytes == nullptr || size < kPackedEnvelopeBytes + 16) {
        return 0;
    }
    return ReadPackedU64(bytes, kPackedEnvelopeBytes + 8);
}

void NoteStateCutDelivery(ProcessOwner& owner, std::uint64_t epoch,
                          std::uint64_t generation, std::uint32_t recordKind,
                          const std::uint8_t* bytes, std::uint32_t size) {
    const std::uint64_t cutId = PackedStateCutId(bytes, size);
    if (cutId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(owner.mutex);
    for (Session& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) ||
            session.generation != generation || session.epoch != epoch) {
            continue;
        }
        if (session.cut.id != cutId) {
            continue;
        }
        if (recordKind == VN_MONITOR_RECORD_STATE_CUT_BEGIN) {
            session.cut.beginDelivered = true;
        } else if (recordKind == VN_MONITOR_RECORD_STATE_CUT_END) {
            session.cut.endDelivered = true;
        }
        return;
    }
}

void SettleStateCutFromDelivery(StateCut* cut,
                                std::uint64_t lastLogicFanoutSequence) {
    if (cut == nullptr || cut->id == 0 || cut->settled) {
        return;
    }
    if (!cut->beginEnqueued && !cut->endEnqueued) {
        cut->result = VN_MONITOR_STATE_CUT_ABORTED;
        cut->checksum = 0;
        cut->emittedCount = 0;
        cut->settled = true;
        return;
    }
    const std::uint64_t lastExpected =
        cut->endEnqueued ? cut->endSourceSequence : cut->beginSourceSequence;
    if (lastLogicFanoutSequence < lastExpected) {
        return;
    }
    if (cut->beginDelivered && cut->endDelivered) {
        cut->result = VN_MONITOR_STATE_CUT_COMPLETE;
        cut->emittedCount = 2;
        cut->settled = true;
        return;
    }
    if (cut->beginDelivered) {
        cut->result = VN_MONITOR_STATE_CUT_TRUNCATED;
        cut->emittedCount = 1;
        cut->settled = true;
        return;
    }
    cut->result = VN_MONITOR_STATE_CUT_ABORTED;
    cut->checksum = 0;
    cut->emittedCount = 0;
    cut->settled = true;
}

Session* LiveSessionMatching(ProcessOwner& owner, ConsumerQueue* queue,
                             std::uint64_t epoch, std::uint64_t generation) {
    for (Session& session : owner.sessions) {
        if (session.live.load(std::memory_order_acquire) &&
            session.generation == generation && session.epoch == epoch &&
            session.queuePtr.load(std::memory_order_acquire) == queue) {
            return &session;
        }
    }
    return nullptr;
}

bool TargetStillLive(ProcessOwner& owner, ConsumerQueue* queue,
                     std::uint64_t epoch, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(owner.mutex);
    return LiveSessionMatching(owner, queue, epoch, generation) != nullptr;
}

// owner.mutex is held across eligibility, enqueue and verification. Cut freeze
// cannot overtake a delta already selected for this session.
void FanoutIdentityLocked(ProcessOwner& owner, const std::uint8_t* bytes, std::uint32_t size) {
    VnMonitorRecordHeader header{};
    if (!ReadIdentityHeader(bytes, size, header)) return;
    const auto cut = PackedStateCutId(bytes, size);
    const auto sequence = header.sourceSequence;
    const bool symbol = header.recordKind == VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL;
    const auto* payload = bytes + kPackedEnvelopeBytes;
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || session.executionFactVersion == 0 ||
            (session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) == 0 || session.eventDrained ||
            session.streamTerminated.load(std::memory_order_acquire) || !session.queue) continue;
        if (symbol && !WantsSymbols(session)) continue;
        if (cut != 0) {
            if (session.cut.id != cut || session.epoch != header.monitorSessionEpoch) continue;
        } else if (session.identity.requested || sequence <= session.identityStart ||
            (session.eventStoppedNs != 0 && sequence > session.identityEnd)) continue;
        FlushDropSummary(owner, *session.queue, VN_MONITOR_DROP_HUB_FANOUT, session.epoch, session.generation, session.identity.active);
        const bool delivered = EnqueueRecord(*session.queue, bytes, size, sequence, header.recordKind,
            VN_MONITOR_DROP_HUB_FANOUT, true, session.epoch, session.generation);
        if (!delivered) {
            if (WantsSymbols(session)) MarkSymbolIncomplete(session);
            session.baselineAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
            if (session.cut.settled && session.cut.result == VN_MONITOR_STATE_CUT_COMPLETE)
                session.cut.result = VN_MONITOR_STATE_CUT_TRUNCATED;
            continue;
        }
        if (cut == 0) continue;
        if (header.recordKind == VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN) {
            VnMonitorIdentityCutBeginV1 value{};
            if (DecodeIdentityBegin(header, payload, header.payloadBytes, value))
                session.cut.beginDelivered = session.identity.delivered.Begin(header, value);
        } else if (header.recordKind == VN_MONITOR_RECORD_IDENTITY_FACT) {
            VnMonitorIdentityFactV1 value{};
            if (DecodeIdentityFact(header, payload, header.payloadBytes, value))
                session.identity.delivered.Row(header, value);
        } else if (symbol) {
            VnMonitorHostObjectSymbolV1 value{};
            if (DecodeHostSymbol(header, payload, header.payloadBytes, value))
                session.identity.delivered.Row(header, value);
        } else {
            VnMonitorIdentityCutEndV1 value{};
            if (DecodeIdentityEnd(header, payload, header.payloadBytes, value)) {
                session.cut.endDelivered = true;
                session.identity.delivered.End(header, value);
            }
        }
    }
}

void FanoutOne(ProcessOwner& owner, const std::uint8_t* bytes, std::uint32_t size,
               MonitorProducerOrigin origin = {}) {
    if (IsIdentityRecord(ReadPackedU32(bytes, 0))) {
        std::lock_guard<std::mutex> lock(owner.mutex);
        FanoutIdentityLocked(owner, bytes, size);
        return;
    }
    const std::uint64_t sourceSequence =
        ReadPackedU64(bytes, kPackedSourceSequenceOffset);
    const std::uint32_t recordKind = ReadPackedU32(bytes, 0);
    const std::uint32_t streamKind = ReadPackedU32(bytes, 12);
    const std::uint64_t recordEpoch =
        ReadPackedU64(bytes, kPackedSessionEpochOffset);
    struct Target {
        ConsumerQueue* queue = nullptr;
        std::uint32_t mask = 0;
        std::uint64_t epoch = 0;
        std::uint64_t generation = 0;
    };
    const std::uint32_t recordFlags = ReadPackedU32(bytes, 8);
    const std::uint32_t recordVersion = ReadPackedU32(bytes, 4);
    const bool unknownKind = recordKind == VN_MONITOR_RECORD_UNKNOWN ||
        (recordKind >= VN_MONITOR_RECORD_INVALID &&
         recordKind != VN_MONITOR_RECORD_EVENT_DETAIL && recordKind != VN_MONITOR_RECORD_EXECUTION_FACT && !IsIdentityRecord(recordKind));
    const bool unknownVersion = recordVersion != 1u &&
        !(recordKind == VN_MONITOR_RECORD_EXECUTION_FACT && recordVersion == 2u);
    const bool unknownRecord = unknownKind || unknownVersion;
    const bool requiredUnknown =
        unknownRecord && (recordFlags & VN_MONITOR_RECORD_FLAG_REQUIRED) != 0;
    Target targets[VN_MONITOR_HARD_SESSIONS_PER_ENGINE];
    std::uint32_t targetCount = 0;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        for (Session& session : owner.sessions) {
            if (!session.live.load(std::memory_order_acquire)) {
                continue;
            }
            if (session.streamTerminated.load(std::memory_order_acquire)) {
                continue;
            }
            if (session.eventDrained) continue;
            if (session.recordingReceiver && recordKind != VN_MONITOR_RECORD_EXECUTION_FACT) {
                if (!TypedRawProjection(bytes, size) || (session.acceptedStreamKindMask & streamKind) == 0) continue;
                if (origin.source == static_cast<unsigned>(RecordingSource::LogicRing) ||
                    origin.source == static_cast<unsigned>(RecordingSource::AudioRing)) {
                    const auto index = origin.source - 1;
                    if (origin.ticket <= session.ringStarts[index] ||
                        (session.eventStoppedNs != 0 && origin.ticket > session.ringEnds[index])) continue;
                } else if (origin.source == static_cast<unsigned>(RecordingSource::AudioLog) ||
                    origin.source == static_cast<unsigned>(RecordingSource::LogicLog)) {
                    const auto index = origin.source - 3;
                    const auto end = index == 0 ? session.sealedAudioLogs : session.sealedLogicLogs;
                    if (origin.ticket <= session.logStarts[index] ||
                        (session.eventStoppedNs != 0 && origin.ticket > end)) continue;
                } else continue;
                FlushDropSummary(owner, *session.queue, VN_MONITOR_DROP_RECORDER_QUEUE, session.epoch, session.generation);
                EnqueueRecord(*session.queue, bytes, size, sourceSequence, recordKind,
                    VN_MONITOR_DROP_RECORDER_QUEUE, true, session.epoch, session.generation, false, origin);
                continue;
            }
            if (recordKind == VN_MONITOR_RECORD_EXECUTION_FACT) {
                if (session.executionFactVersion == 0 || ReadPackedU32(bytes, 4) > session.executionFactVersion) continue;
                const auto producerId = ReadPackedU64(bytes, 32);
                if (producerId != kExecutionLogicProducer && producerId != kExecutionAudioProducer) continue;
                const auto producerIndex = producerId == kExecutionLogicProducer ? 0u : 1u;
                if (sourceSequence <= session.executionStarts[producerIndex] ||
                    (session.eventStoppedNs != 0 && sourceSequence > session.executionEnds[producerIndex])) continue;
            } else if (session.eventCatalog) {
                if (recordKind != VN_MONITOR_RECORD_EVENT_DETAIL) {
                    continue;
                }
                if (session.executionFactVersion != 0 && ExecutionReplacesLegacyEvent(
                    ReadPackedU32(bytes, kPackedEnvelopeBytes + 4),
                    ReadPackedU32(bytes, kPackedEnvelopeBytes + 8))) continue;
                if (session.executionFactVersion != 0 && origin.source >= 3 && origin.source <= 4) {
                    const auto index = origin.source - 3;
                    const auto end = index == 0 ? session.sealedAudioLogs : session.sealedLogicLogs;
                    if (origin.ticket <= session.logStarts[index] ||
                        (session.eventStoppedNs != 0 && origin.ticket > end)) continue;
                } else {
                    const auto timestamp = ReadPackedU64(bytes, kPackedEnvelopeBytes + 24);
                    if (timestamp < session.eventStartedNs ||
                        (session.eventStoppedNs != 0 && timestamp > session.eventStoppedNs)) continue;
                }
            } else if (recordKind == VN_MONITOR_RECORD_EVENT_DETAIL) {
                continue;
            }
            ConsumerQueue* queue = session.queuePtr.load(std::memory_order_acquire);
            if (queue == nullptr) {
                continue;
            }
            targets[targetCount].queue = queue;
            targets[targetCount].mask = session.acceptedStreamKindMask;
            targets[targetCount].epoch = session.epoch;
            targets[targetCount].generation = session.generation;
            ++targetCount;
        }
    }
    owner.fanoutEntered.fetch_add(1, std::memory_order_acq_rel);
    while (owner.fanoutHold.load(std::memory_order_acquire) &&
           !owner.drainStop.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const std::uint64_t recordWorld =
        ReadPackedU64(bytes, kPackedWorldGenerationOffset);
    const std::uint64_t currentWorld =
        owner.currentWorldGeneration.load(std::memory_order_acquire);
    const bool lateWorldState = IsStateCutBoundRecordKind(recordKind) &&
                                !MonitorWorldIsCurrent(currentWorld, recordWorld);
    if (lateWorldState && targetCount > 0) {
        bool seen = false;
        for (std::uint64_t previous : owner.lateWorldGapSeen) {
            if (previous == sourceSequence) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            owner.lateWorldGapSeen[owner.lateWorldGapNext] = sourceSequence;
            owner.lateWorldGapNext =
                (owner.lateWorldGapNext + 1u) %
                static_cast<std::uint32_t>(owner.lateWorldGapSeen.size());
            VnMonitorDropSummary gap{};
            gap.structSize = sizeof(gap);
            gap.stage = VN_MONITOR_DROP_HUB_FANOUT;
            gap.streamId = 1;
            gap.firstSourceSequence = sourceSequence;
            gap.lastSourceSequence = sourceSequence;
            gap.recordCount = 1;
            gap.byteCount = size;
            gap.reason = VN_ACTION_TERMINAL_WORLD_REPLACED;
            std::uint8_t packedGap[kScratchBytes];
            const std::uint64_t gapSequence = owner.nextDropSourceSequence++;
            const std::uint32_t gapSize = PackDropSummaryRecord(
                packedGap, sizeof(packedGap), gap, gapSequence);
            if (gapSize != 0) {
                for (std::uint32_t i = 0; i < targetCount; ++i) {
                    if ((targets[i].mask &
                         (VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE)) == 0) {
                        continue;
                    }
                    EnqueueRecord(*targets[i].queue, packedGap, gapSize, gapSequence,
                                  VN_MONITOR_RECORD_DROP_SUMMARY,
                                  VN_MONITOR_DROP_HUB_FANOUT, true,
                                  targets[i].epoch, targets[i].generation);
                }
            }
        }
    }
    for (std::uint32_t i = 0; i < targetCount; ++i) {
        if ((targets[i].mask & streamKind) == 0) {
            continue;
        }
        if (IsStateCutBoundRecordKind(recordKind) && recordEpoch != 0 &&
            targets[i].epoch != recordEpoch) {
            continue;
        }
        if (lateWorldState) {
            continue;
        }
        if (recordKind == VN_MONITOR_RECORD_AUDIO_CHUNK &&
            owner.liveAudioArmed.load(std::memory_order_acquire) == 0) {
            continue;
        }
        if (recordKind == VN_MONITOR_RECORD_TAP_BINDING_CHANGED) {
            const std::uint64_t recordPlan =
                ReadPackedU64(bytes, kPackedCapturePlanGenerationOffset);
            const std::uint64_t livePlan =
                owner.liveCapturePlanGeneration.load(std::memory_order_acquire);
            if (recordPlan != 0 && livePlan != 0 && recordPlan != livePlan) {
                continue;
            }
        }
        ConsumerQueue* queue = targets[i].queue;
        const std::uint64_t epoch = targets[i].epoch;
        const std::uint64_t generation = targets[i].generation;
        if (!TargetStillLive(owner, queue, epoch, generation)) {
            continue;
        }
        FlushDropSummary(owner, *queue, VN_MONITOR_DROP_HUB_FANOUT, epoch,
                         generation);
        const bool delivered =
            EnqueueRecord(*queue, bytes, size, sourceSequence, recordKind,
                          VN_MONITOR_DROP_HUB_FANOUT, true, epoch, generation, false, origin);
        if (requiredUnknown) {
            std::lock_guard<std::mutex> lock(owner.mutex);
            if (Session* live = LiveSessionMatching(owner, queue, epoch, generation)) {
                live->streamTerminated.store(true, std::memory_order_release);
            }
        }
        if (delivered && IsStateCutRecordKind(recordKind)) {
            NoteStateCutDelivery(owner, epoch, generation, recordKind, bytes,
                                 size);
        }
    }
    static const std::uint32_t kInternalStages[kInternalConsumerCount] = {
        VN_MONITOR_DROP_RECORDER_QUEUE, VN_MONITOR_DROP_REMOTE_TRANSPORT,
        VN_MONITOR_DROP_REMOTE_TRANSPORT};
    for (std::uint32_t i = 0; i < kInternalConsumerCount; ++i) {
        // Legacy recorder/transport queues have no typed owner grant. Typed
        // recording installs its own authorized projection before enabling it.
        if (recordKind == VN_MONITOR_RECORD_EXECUTION_FACT || IsIdentityRecord(recordKind)) continue;
        if ((owner.internalEnabledMask.load(std::memory_order_acquire) &
             (1u << i)) == 0) {
            continue;
        }
        FlushDropSummary(owner, *owner.internals[i], kInternalStages[i]);
        EnqueueRecord(*owner.internals[i], bytes, size, sourceSequence, recordKind,
                      kInternalStages[i], true);
    }
    owner.fanoutCompleted.fetch_add(1, std::memory_order_release);
    if (recordKind == VN_MONITOR_RECORD_EVENT_DETAIL) {
        owner.lastEventFanoutSequence.store(sourceSequence, std::memory_order_release);
    }
    if (IsStateCutRecordKind(recordKind)) {
        owner.lastLogicFanoutSequence.store(sourceSequence,
                                            std::memory_order_release);
    }
}

void ProcessPendingOnDrain(ProcessOwner& owner) {
    std::uint8_t packed[kScratchBytes];
    std::uint32_t size = 0;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        const std::uint64_t now =
            owner.executeFrame.load(std::memory_order_relaxed);
        const std::uint64_t ready =
            owner.pendingReadyFrame.load(std::memory_order_relaxed);
        if (owner.pendingSlot < 0 || now < ready) {
            return;
        }
        const bool pendingStop =
            owner.pendingSlot == static_cast<std::int32_t>(kStopSlotIndex);
        const bool pendingApply = !pendingStop;
        ExpireLeaseIfDueLocked(owner);
        const bool applyLeaseGone = pendingApply &&
            (!owner.lease.held ||
             owner.lease.generation != owner.pendingLeaseGeneration);
        if (applyLeaseGone) {
            PlanSlot& pending =
                owner.planSlots[static_cast<std::size_t>(owner.pendingSlot)];
            pending.derivedState.reset();
            pending.occupancy = SlotOccupancy::Free;
            pending.empty = true;
            FillEmptyPlan(&pending.plan);
            owner.pendingSlot = -1;
            owner.pendingLeaseGeneration = 0;
            owner.publishedPending.store(-1, std::memory_order_release);
            owner.pendingReadyFrame.store(0, std::memory_order_release);
        } else {
            const bool stop = pendingStop;
            ActivatePendingLocked(owner);
            size = PrepareActivationLocked(
                owner, packed, sizeof(packed),
                stop ? kPlanReasonStop : kPlanReasonApply);
            const auto ticket = owner.controlPublished == UINT64_MAX ? 0 : ++owner.controlPublished;
            for (auto& session : owner.sessions) {
                if (!session.live.load(std::memory_order_acquire) || !session.recordingReceiver || session.eventDrained ||
                    session.eventStoppedNs != 0 || (session.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0) continue;
                if (size == 0 || ticket == 0) ++session.controlDrops;
                else EnqueueRecord(*session.queue, packed, size, ReadPackedU64(packed, kPackedSourceSequenceOffset),
                    VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED, VN_MONITOR_DROP_RECORDER_QUEUE, true, session.epoch,
                    session.generation, false, {static_cast<unsigned>(RecordingSource::Control), 0, ticket});
            }
        }
        RetireLingeringLocked(owner);
    }
    if (size > 0) {
        FanoutOne(owner, packed, size);
        owner.publishedCount.fetch_add(1, std::memory_order_relaxed);
    }
}

bool PendingReadyToActivate(const ProcessOwner& owner) {
    if (owner.publishedPending.load(std::memory_order_relaxed) < 0) {
        return false;
    }
    return owner.executeFrame.load(std::memory_order_acquire) >=
           owner.pendingReadyFrame.load(std::memory_order_acquire);
}

void NoteExecutionSourceDrop(ProcessOwner& owner, unsigned producerIndex,
    std::uint64_t first, std::uint64_t last) {
    std::lock_guard<std::mutex> lock(owner.mutex);
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || session.executionFactVersion == 0 || session.eventDrained ||
            (session.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0)
            continue;
        const auto begin = session.executionStarts[producerIndex];
        if (begin == UINT64_MAX) continue;
        const auto clippedFirst = std::max(first, begin + 1);
        const auto clippedLast = session.eventStoppedNs != 0 ? std::min(last, session.executionEnds[producerIndex]) : last;
        if (clippedFirst > clippedLast) continue;
        const auto count = clippedLast - clippedFirst + 1;
        session.eventSourceDrops = UINT64_MAX - session.eventSourceDrops < count ? UINT64_MAX : session.eventSourceDrops + count;
        VnMonitorDropSummary summary{};
        summary.structSize = sizeof(summary);
        summary.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
        summary.streamId = producerIndex == 0 ? kExecutionLogicStream : kExecutionAudioStream;
        summary.firstSourceSequence = clippedFirst;
        summary.lastSourceSequence = clippedLast;
        summary.recordCount = count;
        summary.byteCount = count > UINT64_MAX / sizeof(MonitorExecutionFact) ? UINT64_MAX : count * sizeof(MonitorExecutionFact);
        summary.reason = VN_MONITOR_EVENT_DROP;
        session.executionSourceDrops[producerIndex].NoteRange(clippedFirst, clippedLast,
            count, summary.byteCount, summary.reason);
        std::uint8_t packed[kScratchBytes];
        const auto sequence = owner.nextDropSourceSequence++;
        const auto size = PackDropSummaryRecord(packed, sizeof(packed), summary, sequence);
        if (size != 0 && session.queue) {
            WriteU32(packed + 12, VN_MONITOR_STREAM_EVENT);
            EnqueueRecord(*session.queue, packed, size, sequence, VN_MONITOR_RECORD_DROP_SUMMARY,
                VN_MONITOR_DROP_HUB_FANOUT, true, session.epoch, session.generation);
        }
    }
}

void NoteIdentitySourceDropLocked(ProcessOwner& owner, std::uint64_t first, std::uint64_t last) {
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || session.executionFactVersion == 0 ||
            (session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) == 0 || session.eventDrained ||
            session.identity.requested || session.identityStart == UINT64_MAX) continue;
        const auto lo = std::max(first, session.identityStart + 1);
        const auto hi = session.eventStoppedNs == 0 ? last : std::min(last, session.identityEnd);
        if (lo > hi) continue;
        const auto count = hi - lo + 1;
        session.identityDrops.NoteRange(lo, hi, count,
            count > UINT64_MAX / kIdentityRecordBytes ? UINT64_MAX : count * kIdentityRecordBytes, VN_MONITOR_EVENT_DROP);
        session.baselineAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
        if (session.cut.settled && session.cut.result == VN_MONITOR_STATE_CUT_COMPLETE)
            session.cut.result = VN_MONITOR_STATE_CUT_TRUNCATED;
        VnMonitorDropSummary summary{};
        summary.structSize = sizeof(summary);
        summary.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
        summary.streamId = kIdentityDeltaStream;
        summary.firstSourceSequence = lo;
        summary.lastSourceSequence = hi;
        summary.recordCount = count;
        summary.byteCount = count > UINT64_MAX / kIdentityRecordBytes ? UINT64_MAX : count * kIdentityRecordBytes;
        summary.reason = VN_MONITOR_EVENT_DROP;
        std::uint8_t bytes[kScratchBytes]{};
        const auto sequence = owner.nextDropSourceSequence++;
        const auto size = PackDropSummaryRecord(bytes, sizeof(bytes), summary, sequence);
        if (size != 0 && session.queue) {
            WriteU32(bytes + 12, VN_MONITOR_STREAM_STATE);
            EnqueueRecord(*session.queue, bytes, size, sequence, VN_MONITOR_RECORD_DROP_SUMMARY,
                VN_MONITOR_DROP_HUB_FANOUT, true, session.epoch, session.generation);
        }
    }
}

bool DrainIdentityBaseline(ProcessOwner& owner, std::uint8_t* scratch, std::uint32_t scratchBytes) {
    std::uint32_t size = 0;
    std::uint64_t epoch = 0, generation = 0, cut = 0;
    bool ending = false;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        for (auto& session : owner.sessions) {
            auto& stage = session.identity;
            if (!session.live.load(std::memory_order_acquire) || !stage.active) continue;
            epoch = session.epoch;
            generation = session.generation;
            cut = stage.begin.stateCutId;
            const auto sequence = stage.firstSequence + stage.cursor;
            std::uint32_t expectedBytes = kIdentityRecordBytes;
            if (stage.cursor == 0) {
                expectedBytes = PackedRecordBytes(kIdentityBeginBytes);
                const auto header = IdentityCutHeader(stage.engine, sequence, epoch,
                    VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, kIdentityBeginBytes);
                size = PackIdentityBegin(scratch, scratchBytes, header, stage.begin);
            } else if (stage.cursor <= stage.identityCount) {
                auto fact = stage.rows[stage.cursor - 1];
                fact.sourceSequence = sequence;
                size = PackIdentityFact(scratch, scratchBytes, fact, cut, stage.begin.identityRevision);
                if (size != 0) {
                    WriteU64(scratch + kPackedSessionEpochOffset, epoch);
                    VnMonitorRecordHeader header{};
                    VnMonitorIdentityFactV1 value{};
                    if (ReadIdentityHeader(scratch, size, header) &&
                        DecodeIdentityFact(header, scratch + kPackedEnvelopeBytes, header.payloadBytes, value)) {
                        stage.checksum = IdentityRowChecksum(stage.checksum, header, value);
                        ++stage.emitted[value.fact.subjectNamespace];
                    } else size = 0;
                }
                if (owner.dropIdentityRowForTests == static_cast<std::int32_t>(stage.cursor - 1)) {
                    owner.dropIdentityRowForTests = -1;
                    size = 0;
                }
            } else if (stage.cursor <= stage.count) {
                expectedBytes = kHostSymbolRecordBytes;
                const auto fieldIndex = stage.cursor - stage.identityCount - 1;
                auto source = HostSymbolField(stage.symbols[fieldIndex / 3], fieldIndex % 3 + 1,
                    VN_MONITOR_SYMBOL_UPSERT, stage.begin.identityRevision);
                source.sourceSequence = sequence;
                size = PackHostSymbol(scratch, scratchBytes, source, cut, VN_MONITOR_STREAM_STATE);
                if (size != 0) {
                    WriteU64(scratch + kPackedSessionEpochOffset, epoch);
                    VnMonitorRecordHeader header{};
                    VnMonitorHostObjectSymbolV1 value{};
                    if (ReadIdentityHeader(scratch, size, header) &&
                        DecodeHostSymbol(header, scratch + kPackedEnvelopeBytes, header.payloadBytes, value)) {
                        stage.checksum = HostSymbolRowChecksum(stage.checksum, header, value);
                        ++stage.emitted[VN_MONITOR_SUBJECT_HOST_SYMBOL];
                    } else size = 0;
                }
                if (owner.dropIdentityRowForTests == static_cast<std::int32_t>(stage.cursor - 1)) {
                    owner.dropIdentityRowForTests = -1;
                    size = 0;
                }
            } else {
                expectedBytes = PackedRecordBytes(kIdentityEndBytes);
                ending = true;
                VnMonitorIdentityCutEndV1 value{};
                value.structSize = sizeof(value);
                value.stateCutId = cut;
                value.identityRevision = stage.begin.identityRevision;
                value.result = stage.sourceComplete ? VN_MONITOR_STATE_CUT_COMPLETE : VN_MONITOR_STATE_CUT_TRUNCATED;
                value.checksum = stage.checksum;
                std::copy(std::begin(stage.emitted), std::end(stage.emitted), value.namespaceEmittedCount);
                const auto header = IdentityCutHeader(stage.engine, sequence, epoch,
                    VN_MONITOR_RECORD_IDENTITY_CUT_END, kIdentityEndBytes);
                size = PackIdentityEnd(scratch, scratchBytes, header, value);
            }
            if (size == 0) {
                stage.sourceComplete = false;
                session.baselineDrops.NoteRange(sequence, sequence, 1, expectedBytes, VN_MONITOR_EVENT_DROP);
            }
            ++stage.cursor;
            break;
        }
    }
    if (cut == 0) return false;
    if (size != 0) FanoutOne(owner, scratch, size);
    if (ending) {
        std::lock_guard<std::mutex> lock(owner.mutex);
        for (auto& session : owner.sessions) {
            if (!session.live.load(std::memory_order_acquire) || session.epoch != epoch ||
                session.generation != generation || session.cut.id != cut) continue;
            auto& stage = session.identity;
            stage.active = false;
            {
                std::lock_guard<std::mutex> queueLock(session.queue->mutex);
                session.queue->symbolBaselineActive = false;
            }
            session.cut.settled = true;
            session.cut.emittedCount = static_cast<std::uint32_t>(stage.delivered.Received());
            session.cut.checksum = stage.delivered.Checksum();
            const bool complete = stage.sourceComplete && stage.delivered.Complete() &&
                session.baselineAvailability != VN_MONITOR_OBSERVATION_INCOMPLETE;
            session.cut.result = complete ? VN_MONITOR_STATE_CUT_COMPLETE :
                (session.cut.beginDelivered ? VN_MONITOR_STATE_CUT_TRUNCATED : VN_MONITOR_STATE_CUT_ABORTED);
            session.baselineAvailability = complete ? VN_MONITOR_OBSERVATION_AVAILABLE : VN_MONITOR_OBSERVATION_INCOMPLETE;
            if (WantsSymbols(session)) session.symbolAvailability = session.baselineAvailability;
        }
    }
    return true;
}

bool DrainIdentityOne(ProcessOwner& owner, std::uint8_t* scratch, std::uint32_t scratchBytes) {
    // All immutable baselines precede deltas. Each session then filters deltas
    // by its own frozen source start, independent of other sessions' cut ranges.
    if (DrainIdentityBaseline(owner, scratch, scratchBytes)) return true;
    if (!owner.identityResourcesReady.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(owner.mutex);
    for (const auto& session : owner.sessions)
        if (session.live.load(std::memory_order_acquire) && session.identity.active) return true;
    auto& producer = owner.identityProducer;
    const auto drop = [&](std::uint64_t first, std::uint64_t last) { NoteIdentitySourceDropLocked(owner, first, last); };
    const auto dispatch = [&] {
        return producer.DispatchPending([&](const MonitorIdentityDelta& delta) {
            auto fact = delta.fact;
            fact.sourceSequence = delta.sourceSequence;
            const auto size = PackIdentityFact(scratch, scratchBytes, fact, 0, delta.revision);
            if (size == 0) return false;
            FanoutIdentityLocked(owner, scratch, size);
            return true;
        }, drop);
    };
    if (producer.HasPending()) return dispatch();
    const auto published = producer.Published();
    std::uint32_t size = 0;
    if (!owner.identityRing.TryPop(scratch, scratchBytes, &size) || size == 0)
        return producer.FinishEmpty(published, drop);
    if (!producer.Stage(scratch, size)) return false;
    return dispatch();
}

bool SymbolWindow(const Session& session, std::uint64_t sequence) {
    return session.live.load(std::memory_order_acquire) && WantsSymbols(session) &&
        !session.eventDrained && !session.streamTerminated.load(std::memory_order_acquire) &&
        !session.identity.requested && sequence > session.symbolStart &&
        (session.eventStoppedNs == 0 || sequence <= session.symbolEnd) && session.queue;
}

bool DeferSymbols(const Session& session) {
    std::lock_guard<std::mutex> lock(session.queue->mutex);
    return session.identity.active || !session.queue->deferredSymbols.empty();
}

void NoteSymbolSourceDropLocked(ProcessOwner& owner, std::uint64_t first, std::uint64_t last) {
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || !WantsSymbols(session) ||
            session.eventDrained || session.identity.requested || session.symbolStart == UINT64_MAX) continue;
        const auto lo = std::max(first, session.symbolStart + 1);
        const auto hi = session.eventStoppedNs == 0 ? last : std::min(last, session.symbolEnd);
        if (lo > hi) continue;
        const auto count = hi - lo + 1;
        const auto bytes = count > UINT64_MAX / kHostSymbolRecordBytes ? UINT64_MAX : count * kHostSymbolRecordBytes;
        session.symbolDrops.NoteRange(lo, hi, count, bytes, VN_MONITOR_EVENT_DROP);
        MarkSymbolIncomplete(session);
        VnMonitorDropSummary summary{};
        summary.structSize = sizeof(summary);
        summary.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
        summary.streamId = kHostSymbolStream;
        summary.firstSourceSequence = lo;
        summary.lastSourceSequence = hi;
        summary.recordCount = count;
        summary.byteCount = bytes;
        summary.reason = VN_MONITOR_EVENT_DROP;
        std::uint8_t packed[kScratchBytes]{};
        const auto sequence = owner.nextDropSourceSequence++;
        const auto size = PackDropSummaryRecord(packed, sizeof(packed), summary, sequence);
        if (size != 0 && session.queue) {
            WriteU32(packed + 12, SymbolStreamKind(session));
            EnqueueRecord(*session.queue, packed, size, sequence, VN_MONITOR_RECORD_DROP_SUMMARY,
                VN_MONITOR_DROP_HUB_FANOUT, false, session.epoch, session.generation, DeferSymbols(session));
        }
    }
}

bool DrainHostSymbolOne(ProcessOwner& owner, std::uint8_t* scratch, std::uint32_t scratchBytes) {
    if (!owner.symbolResourcesReady.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(owner.mutex);
    auto& producer = owner.symbolProducer;
    const auto drop = [&](std::uint64_t first, std::uint64_t last) { NoteSymbolSourceDropLocked(owner, first, last); };
    const auto dispatch = [&] {
        return producer.DispatchPending([&](const MonitorHostSymbolSource& source) {
            // Validate even if every destination has closed or excludes this window.
            if (PackHostSymbol(scratch, scratchBytes, source, 0, VN_MONITOR_STREAM_EVENT) == 0) return false;
            for (auto& session : owner.sessions) {
                if (!SymbolWindow(session, source.sourceSequence)) continue;
                const bool defer = DeferSymbols(session);
                FlushDropSummary(owner, *session.queue, VN_MONITOR_DROP_HUB_FANOUT, session.epoch, session.generation, defer);
                const auto size = PackHostSymbol(scratch, scratchBytes, source, 0, SymbolStreamKind(session));
                WriteU64(scratch + kPackedSessionEpochOffset, session.epoch);
                if (!EnqueueRecord(*session.queue, scratch, size, source.sourceSequence,
                    VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL, VN_MONITOR_DROP_HUB_FANOUT, true,
                    session.epoch, session.generation, defer)) MarkSymbolIncomplete(session);
            }
            return true;
        }, drop);
    };
    if (producer.HasPending()) return dispatch();
    const auto published = producer.Published();
    std::uint32_t size = 0;
    if (!owner.symbolRing.TryPop(scratch, scratchBytes, &size) || size == 0)
        return producer.FinishEmpty(published, drop);
    if (!producer.Stage(scratch, size)) return false;
    return dispatch();
}

bool DrainDeferredSymbols(ProcessOwner& owner) {
    std::lock_guard<std::mutex> lock(owner.mutex);
    bool moved = false;
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || session.identity.active ||
            session.identity.requested || !session.queue) continue;
        auto& queue = *session.queue;
        std::lock_guard<std::mutex> queueLock(queue.mutex);
        if (queue.deferredSymbols.empty()) continue;
        auto record = std::move(queue.deferredSymbols.front());
        queue.deferredSymbols.pop_front();
        StampDelivery(record.bytes.data(), static_cast<std::uint32_t>(record.bytes.size()), queue.nextDelivery++);
        queue.records.push_back(std::move(record));
        moved = true;
    }
    return moved;
}

// Every path gets one bounded opportunity; a long cut cannot starve EVENT-only symbols.
bool DrainTypedOne(ProcessOwner& owner, std::uint8_t* scratch, std::uint32_t scratchBytes) {
    const bool identity = DrainIdentityOne(owner, scratch, scratchBytes);
    const bool symbol = DrainHostSymbolOne(owner, scratch, scratchBytes);
    const bool deferred = DrainDeferredSymbols(owner);
    return identity || symbol || deferred;
}

bool DrainOneFrom(ProcessOwner& owner, MonitorProducerRing& ring,
                  std::uint8_t* scratch, std::uint32_t scratchBytes) {
    const unsigned producerIndex = &ring == &owner.logicRing ? 0u : 1u;
    auto& execution = owner.executionProducers[producerIndex];
    const auto drop = [&](std::uint64_t first, std::uint64_t last) {
        NoteExecutionSourceDrop(owner, producerIndex, first, last);
    };
    const auto dispatch = [&] {
        const bool progressed = execution.DispatchPending([&](const MonitorExecutionFact& fact) {
            const auto size = PackExecutionFact(scratch, scratchBytes, fact,
                producerIndex == 0 ? kExecutionLogicProducer : kExecutionAudioProducer,
                producerIndex == 0 ? kExecutionLogicStream : kExecutionAudioStream);
            if (size == 0) return false;
            FanoutOne(owner, scratch, size);
            return true;
        }, drop);
        if (!execution.HasPending() && owner.pendingRingTickets[producerIndex] != 0) {
            owner.ringProjectionCompleted[producerIndex].store(owner.pendingRingTickets[producerIndex], std::memory_order_release);
            owner.pendingRingTickets[producerIndex] = 0;
        }
        return progressed;
    };
    if (execution.HasPending()) return dispatch();
    const auto publishedBeforeRead = execution.Published();
    std::uint32_t size = 0;
    std::uint32_t sourceKind = 0;
    MonitorProducerOrigin origin{};
    std::uint64_t ticket = 0;
    if (!ring.TryPop(scratch, scratchBytes, &size, &sourceKind, &origin, &ticket) || size == 0) {
        return execution.FinishEmpty(publishedBeforeRead, drop);
    }
    owner.drainEntered.fetch_add(1, std::memory_order_acq_rel);
    while (owner.drainHold.load(std::memory_order_acquire) &&
           !owner.drainStop.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (sourceKind == kExecutionSourceKind) {
        owner.pendingRingTickets[producerIndex] = ticket;
        if (!execution.Stage(scratch, size)) {
            owner.ringProjectionCompleted[producerIndex].store(ticket, std::memory_order_release);
            owner.pendingRingTickets[producerIndex] = 0;
            return true;
        }
        return dispatch();
    }
    if (origin.source == 0) origin = {producerIndex + 1, 0, ticket};
    FanoutOne(owner, scratch, size, origin);
    const std::uint32_t recordKind = ReadPackedU32(scratch, 0);
    if (recordKind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
        std::array<VnMonitorMeasurementRequest,
                   VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN>
            measurements{};
        std::array<VnMonitorTapDescriptor, VN_MONITOR_HARD_TAPS_PER_PLAN> taps{};
        std::shared_ptr<DerivedDrainState> derivedState;
        std::uint32_t measurementCount = 0;
        std::uint32_t tapCount = 0;
        std::uint64_t activeCapturePlanGeneration = 0;
        std::uint64_t effectiveFrom = 0;
        std::uint32_t audioArmed = 0;
        {
            std::lock_guard<std::mutex> lock(owner.mutex);
            audioArmed = owner.liveAudioArmed.load(std::memory_order_relaxed);
            if (owner.activeSlot >= 0) {
                const PlanSlot& slot =
                    owner.planSlots[static_cast<std::size_t>(owner.activeSlot)];
                derivedState = slot.derivedState;
                measurementCount = slot.storedMeasurementCount;
                tapCount = slot.storedTapCount;
                measurements = slot.measurements;
                taps = slot.taps;
                activeCapturePlanGeneration = owner.capturePlanGeneration;
                effectiveFrom = owner.effectiveFromExecuteFrame;
            }
        }
        const std::uint64_t recordPlan =
            ReadPackedU64(scratch, kPackedCapturePlanGenerationOffset);
        const std::uint64_t recordFrame =
            ReadPackedU64(scratch, kPackedExecuteFrameOffset);
        const bool beforeEffective =
            recordPlan == activeCapturePlanGeneration && recordFrame < effectiveFrom;
        if (audioArmed != 0 && !beforeEffective && measurementCount != 0 &&
            tapCount != 0 && derivedState) {
            struct EmitCtx {
                ProcessOwner* owner = nullptr;
                MonitorProducerOrigin origin{};
            } ctx;
            ctx.owner = &owner;
            ctx.origin = origin;
            derivedState->Dispatch(
                measurements.data(), measurementCount, taps.data(), tapCount,
                scratch, size,
                [](void* raw, const DerivedEmit& emit) {
                    auto* typed = static_cast<EmitCtx*>(raw);
                    if (typed == nullptr || typed->owner == nullptr ||
                        emit.bytes == nullptr || emit.size == 0) {
                        return false;
                    }
                    FanoutOne(*typed->owner, emit.bytes, emit.size, typed->origin);
                    return true;
                },
                &ctx, activeCapturePlanGeneration);
        }
    }
    owner.ringProjectionCompleted[producerIndex].store(ticket, std::memory_order_release);
    return true;
}

std::uint32_t RecordingErrorCode(VnResult result) {
    return result < 0 ? static_cast<std::uint32_t>(-result)
                      : static_cast<std::uint32_t>(result);
}

void FailRecorderLocked(ProcessOwner::Recorder& recorder, VnResult reason) {
    recorder.state = VN_MONITOR_RECORDING_STATE_FAILED;
    recorder.errorReason = RecordingErrorCode(reason);
    recorder.recoveryNeeded = 1;
    if (recorder.file.is_open()) {
        recorder.file.close();
    }
}

VnResult RecorderFailureResult(const ProcessOwner::Recorder& recorder) {
    if (recorder.errorReason == 0) {
        return VN_MONITOR_RECORDING_FAILED;
    }
    return static_cast<VnResult>(-static_cast<VnResult>(recorder.errorReason));
}

VnResult FailAndRemoveStartFileLocked(ProcessOwner::Recorder& recorder,
                                      VnResult reason) {
    FailRecorderLocked(recorder, reason);
    if (!recorder.path.empty()) {
        std::error_code ec;
        std::filesystem::remove(recorder.path, ec);
    }
    return reason;
}

void AbandonIncompleteRecordingLocked(ProcessOwner::Recorder& recorder) {
    if (recorder.state != VN_MONITOR_RECORDING_STATE_ACTIVE &&
        recorder.state != VN_MONITOR_RECORDING_STATE_STOPPING) {
        return;
    }
    FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
}

bool WriteRecorderChunkLocked(ProcessOwner::Recorder& recorder, std::uint16_t kind,
                              std::uint16_t flags, const std::uint8_t* payload,
                              std::uint64_t payloadBytes) {
    if (!recorder.file.is_open()) {
        FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
        return false;
    }
    PackSessionChunk(&recorder.packBuffer, kind, flags, recorder.nextChunk, payload,
                     payloadBytes);
    const std::uint64_t packedBytes = recorder.packBuffer.size();
    const std::uint64_t nextBytes = recorder.fileBytes + packedBytes;
    if (recorder.fileByteBudget != 0 && nextBytes > recorder.fileByteBudget) {
        FailRecorderLocked(recorder, VN_MONITOR_BUDGET_EXCEEDED);
        return false;
    }
    if (recorder.failNextWriteForTests) {
        recorder.failNextWriteForTests = false;
        FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
        return false;
    }
    recorder.file.write(reinterpret_cast<const char*>(recorder.packBuffer.data()),
                        static_cast<std::streamsize>(packedBytes));
    if (!recorder.file.good()) {
        FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
        return false;
    }
    recorder.unflushedBytes += packedBytes;
    constexpr std::uint64_t kFlushBytes = 65536;
    if (recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING ||
        recorder.unflushedBytes >= kFlushBytes) {
        recorder.file.flush();
        if (!recorder.file.good()) {
            FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
            return false;
        }
        recorder.unflushedBytes = 0;
    }
    recorder.fileBytes = nextBytes;
    recorder.lastCommittedChunk = recorder.nextChunk;
    recorder.nextChunk += 1;
    return true;
}

bool TryWriteOptionalRecorderChunkLocked(ProcessOwner::Recorder& recorder,
                                         std::uint16_t kind, std::uint16_t flags,
                                         const std::uint8_t* payload,
                                         std::uint64_t payloadBytes) {
    if (!recorder.file.is_open()) {
        return false;
    }
    PackSessionChunk(&recorder.packBuffer, kind, flags, recorder.nextChunk, payload,
                     payloadBytes);
    const std::uint64_t packedBytes = recorder.packBuffer.size();
    const std::uint64_t nextBytes = recorder.fileBytes + packedBytes;
    if (recorder.fileByteBudget != 0 && nextBytes > recorder.fileByteBudget) {
        return false;
    }
    if (recorder.unflushedBytes != 0) {
        recorder.file.flush();
        if (!recorder.file.good()) {
            FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
            return false;
        }
        recorder.unflushedBytes = 0;
    }
    auto restoreOptionalWrite = [&]() -> bool {
        recorder.file.clear();
        recorder.file.close();
        std::error_code resizeEc;
        if (recorder.failOptionalRestoreForTests) {
            recorder.failOptionalRestoreForTests = false;
            FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
            return false;
        }
        std::filesystem::resize_file(recorder.path, recorder.fileBytes, resizeEc);
        if (resizeEc) {
            FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
            return false;
        }
        recorder.file.open(recorder.path, std::ios::binary | std::ios::app);
        if (!recorder.file.is_open() || !recorder.file.good()) {
            FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
            return false;
        }
        recorder.file.clear();
        return true;
    };
    if (recorder.failNextOptionalWriteForTests) {
        recorder.failNextOptionalWriteForTests = false;
        const std::streamsize half = static_cast<std::streamsize>(packedBytes / 2);
        if (half > 0) {
            recorder.file.write(reinterpret_cast<const char*>(recorder.packBuffer.data()),
                                half);
        }
        restoreOptionalWrite();
        return false;
    }
    recorder.file.write(reinterpret_cast<const char*>(recorder.packBuffer.data()),
                        static_cast<std::streamsize>(packedBytes));
    if (!recorder.file.good()) {
        restoreOptionalWrite();
        return false;
    }
    recorder.unflushedBytes += packedBytes;
    recorder.fileBytes = nextBytes;
    recorder.lastCommittedChunk = recorder.nextChunk;
    recorder.nextChunk += 1;
    return true;
}

bool RecordingSourcesDrained(const ProcessOwner& owner, const Session& session, bool forced) {
    if (session.eventStoppedNs == 0 || (session.sealRequested && !forced)) return false;
    const auto streams = session.acceptedStreamKindMask;
    if ((streams & VN_MONITOR_STREAM_EVENT) != 0) {
        for (unsigned i = 0; i != 2; ++i) {
            if (session.executionEnds[i] > session.executionStarts[i] &&
                owner.executionProducers[i].Completed() < session.executionEnds[i]) return false;
            const auto logEnd = i == 0 ? session.sealedAudioLogs : session.sealedLogicLogs;
            if (!forced && logEnd > session.logStarts[i] && (owner.logProjectionCompleted[i].load(std::memory_order_acquire) < logEnd ||
                owner.ringProjectionCompleted[0].load(std::memory_order_acquire) < session.logDownstreamEnds[i])) return false;
        }
    }
    if ((streams & VN_MONITOR_STREAM_STATE) != 0 && (session.identity.requested || session.identity.active ||
        (session.identityEnd > session.identityStart && owner.identityProducer.Completed() < session.identityEnd))) return false;
    if (WantsSymbols(session) && session.symbolEnd > session.symbolStart && owner.symbolProducer.Completed() < session.symbolEnd) return false;
    for (unsigned i = 0; i != 2; ++i)
        if (session.ringEnds[i] > session.ringStarts[i] &&
            owner.ringProjectionCompleted[i].load(std::memory_order_acquire) < session.ringEnds[i]) return false;
    return true;
}

RecordingEnd RecordingEndSnapshot(const ProcessOwner& owner, const Session& session,
    const ProcessOwner::Recorder& recorder) {
    RecordingEnd end{};
    const auto& descriptor = recorder.descriptor;
    end.engine = descriptor.engine;
    end.epoch = descriptor.epoch;
    end.recordingEpoch = descriptor.recordingEpoch;
    end.stoppedNs = session.eventStoppedNs;
    end.stopReason = recorder.stopReason;
    end.controlEnd = (session.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) != 0 ? session.controlEnd : descriptor.controlStart;
    end.controlDrops = session.controlDrops;
    end.boundaryComplete = recorder.forceIncomplete ? 0u : 1u;
    const auto streams = session.acceptedStreamKindMask;
    for (unsigned i = 0; i != 4; ++i) {
        const bool subscribed = i < 2 ? (streams & VN_MONITOR_STREAM_EVENT) != 0 :
            i == 2 ? (streams & VN_MONITOR_STREAM_STATE) != 0 : WantsSymbols(session);
        end.ends[i] = !subscribed ? descriptor.starts[i] : i < 2 ? session.executionEnds[i] :
            i == 2 ? session.identityEnd : session.symbolEnd;
        end.completed[i] = i < 2 ? owner.executionProducers[i].Completed() :
            i == 2 ? owner.identityProducer.Completed() : owner.symbolProducer.Completed();
        end.unavailable[i] = subscribed && session.unavailableAtEnd[i] > descriptor.unavailableStarts[i]
            ? session.unavailableAtEnd[i] - descriptor.unavailableStarts[i] : 0;
    }
    for (unsigned i = 0; i != 2; ++i) {
        end.sourceDrops[i + 1] = session.executionSourceDrops[i].recordCount;
        end.ringEnds[i] = session.ringEnds[i];
        end.ringCompleted[i] = owner.ringProjectionCompleted[i].load(std::memory_order_acquire);
        end.rawDrops[i] = session.rawDropsAtEnd[i] - descriptor.rawDropStarts[i];
        end.logEnds[i] = (streams & VN_MONITOR_STREAM_EVENT) == 0 ? descriptor.logStarts[i] :
            i == 0 ? session.sealedAudioLogs : session.sealedLogicLogs;
        const bool downstream = owner.ringProjectionCompleted[0].load(std::memory_order_acquire) >= session.logDownstreamEnds[i];
        end.logCompleted[i] = downstream ? owner.logProjectionCompleted[i].load(std::memory_order_acquire) : descriptor.logStarts[i];
        end.nativeLogDrops[i] = (streams & VN_MONITOR_STREAM_EVENT) != 0 ? session.logLost[i] +
            (i == 0 ? session.logBudgetDropsAtEnd - session.nativeLogDrops[0] : 0) : 0;
        end.logFiltered[i] = session.logFiltered[i];
    }
    end.sourceDrops[3] = session.identityDrops.recordCount;
    end.sourceDrops[4] = session.baselineDrops.recordCount;
    end.sourceDrops[5] = session.symbolDrops.recordCount;
    std::copy(session.queue->cumulativeDrops.begin(), session.queue->cumulativeDrops.end(), end.deliveryDrops);
    end.eventSourceDrops = session.eventSourceDrops;
    end.baselineAvailability = session.baselineAvailability;
    end.symbolAvailability = session.symbolAvailability;
    end.recordCount = recorder.verifier.Records();
    end.checksum = recorder.verifier.Checksum();
    return end;
}

void PumpRecorder(ProcessOwner& owner) {
    // A finite batch leaves time for source drain even during sustained recording.
    for (unsigned batch = 0; batch != 64; ++batch) {
        std::unique_lock<std::mutex> ownerLock(owner.mutex);
        ExpireLeaseIfDueLocked(owner);
        std::unique_lock<std::mutex> recorderLock(owner.recorder.mutex);
        auto& recorder = owner.recorder;
        if (recorder.state != VN_MONITOR_RECORDING_STATE_ACTIVE &&
            recorder.state != VN_MONITOR_RECORDING_STATE_STOPPING) return;
        auto* session = recorder.typed ? FindLive(owner, recorder.receiver, true) : nullptr;
        auto* queue = recorder.typed ? (session ? session->queue.get() : nullptr) : owner.internals[kInternalRecorder].get();
        if (!queue) return;
        PackedRecord record;
        bool finish = false;
        {
            std::lock_guard<std::mutex> queueLock(queue->mutex);
            if (!queue->records.empty()) {
                record = std::move(queue->records.front());
                queue->records.pop_front();
                queue->usedBytes -= record.bytes.size();
            } else {
                finish = recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING &&
                    (!recorder.typed || (queue->deferredSymbols.empty() && RecordingSourcesDrained(owner, *session, recorder.forceIncomplete)));
                if (!finish) return;
                if (recorder.typed) {
                    recorder.end = RecordingEndSnapshot(owner, *session, recorder);
                    session->eventDrained = true;
                }
            }
        }
        ownerLock.unlock();
        bool written = true;
        if (!record.bytes.empty()) {
            if (recorder.typed) {
                recorder.frameBuffer.resize(kRecordingPositionBytes + record.bytes.size());
                PackRecordingPosition(recorder.frameBuffer.data(), {static_cast<RecordingSource>(record.origin.source),
                    0, record.origin.ticket, record.bytes.size()});
                std::memcpy(recorder.frameBuffer.data() + kRecordingPositionBytes, record.bytes.data(), record.bytes.size());
                written = recorder.verifier.Record(recorder.frameBuffer.data(), recorder.frameBuffer.size()) &&
                    WriteRecorderChunkLocked(recorder, VN_MONITOR_CHUNK_EXECUTION_RECORD, VN_MONITOR_CHUNK_REQUIRED,
                        recorder.frameBuffer.data(), recorder.frameBuffer.size());
            } else written = WriteRecorderChunkLocked(recorder, VN_MONITOR_CHUNK_EVENTS, VN_MONITOR_CHUNK_REQUIRED,
                record.bytes.data(), record.bytes.size());
        } else if (finish) {
            if (recorder.typed) {
                std::uint8_t packed[kRecordingEndBytes]{};
                ExecutionPayloadWriter write{packed};
                VisitRecordingEnd(write, recorder.end);
                written = recorder.verifier.End(recorder.end) && WriteRecorderChunkLocked(recorder,
                    VN_MONITOR_CHUNK_EXECUTION_END, VN_MONITOR_CHUNK_REQUIRED, packed, sizeof(packed));
            }
            if (written) {
                recorder.file.flush();
                written = recorder.file.good();
            }
            if (written) {
                recorder.file.close();
                recorder.unflushedBytes = 0;
                recorder.state = VN_MONITOR_RECORDING_STATE_STOPPED;
                recorder.errorReason = 0;
                recorder.recoveryNeeded = recorder.typed && !recorder.verifier.Complete() ? 1u : 0u;
                owner.internalEnabledMask.fetch_and(~(1u << kInternalRecorder), std::memory_order_release);
                if (recorder.typed) owner.recorderLogicPending.store(true, std::memory_order_release);
                return;
            }
        }
        if (!written) {
            if (recorder.state != VN_MONITOR_RECORDING_STATE_FAILED) FailRecorderLocked(recorder, VN_MONITOR_RECORDING_FAILED);
            owner.internalEnabledMask.fetch_and(~(1u << kInternalRecorder), std::memory_order_release);
            const bool typed = recorder.typed;
            const auto failedReceiver = recorder.receiver;
            const auto failedEpoch = recorder.queueEpoch;
            const auto beforeCleanup = recorder.beforeFailureCleanupForTests;
            auto* cleanupContext = recorder.failureCleanupContextForTests;
            recorder.beforeFailureCleanupForTests = nullptr;
            recorder.failureCleanupContextForTests = nullptr;
            recorder.identityCleanupPending |= typed;
            recorderLock.unlock();
            if (beforeCleanup) beforeCleanup(cleanupContext);
            // Reenter the owner only after releasing the writer lock.
            std::lock_guard<std::mutex> failedLock(owner.mutex);
            std::lock_guard<std::mutex> failedRecorderLock(owner.recorder.mutex);
            if (owner.recorder.queueEpoch != failedEpoch || owner.recorder.typed != typed ||
                owner.recorder.receiver != failedReceiver) return;
            if (typed) {
                if (auto* receiver = FindLive(owner, failedReceiver, true)) receiver->eventDrained = true;
                owner.recorderLogicPending.store(true, std::memory_order_release);
            } else queue->Detach();
            return;
        }
    }
    owner.WakeDrain();
}

bool DrainOneRound(ProcessOwner& owner, std::uint8_t* scratch, std::uint32_t scratchBytes) {
    const bool typed = DrainTypedOne(owner, scratch, scratchBytes);
    const bool logic = DrainOneFrom(owner, owner.logicRing, scratch, scratchBytes);
    const bool audio = DrainOneFrom(owner, owner.producerRing, scratch, scratchBytes);
    return typed || logic || audio;
}

void DrainLoop(ProcessOwner* owner) {
    if (owner->drainScratch.size() < PackedProducerSlotBytes()) {
        owner->drainScratch.assign(PackedProducerSlotBytes(), 0);
    }
    std::uint8_t* scratch = owner->drainScratch.data();
    const std::uint32_t scratchBytes =
        static_cast<std::uint32_t>(owner->drainScratch.size());
    while (!owner->drainStop.load(std::memory_order_acquire)) {
        const std::uint64_t wakeGeneration =
            owner->drainWakeGeneration.load(std::memory_order_acquire);
        if (PendingReadyToActivate(*owner)) {
            ProcessPendingOnDrain(*owner);
        }
        if (DrainOneRound(*owner, scratch, scratchBytes)) {
            PumpRecorder(*owner);
            continue;
        }
        PumpRecorder(*owner);
        if (!owner->drainStop.load(std::memory_order_acquire) &&
            owner->logicRing.Empty() && owner->producerRing.Empty() && owner->identityRing.Empty() && owner->symbolRing.Empty() &&
            !PendingReadyToActivate(*owner)) {
            owner->WaitForDrainWake(wakeGeneration);
        }
    }
    while (DrainOneRound(*owner, scratch, scratchBytes)) {
    }
}

void StopDrain(ProcessOwner& owner) {
    owner.drainStop.store(true, std::memory_order_release);
    owner.WakeDrain();
    if (owner.drainThread.joinable()) {
        owner.drainThread.join();
    }
    owner.drainRunning.store(false, std::memory_order_release);
}

void ReleaseCaptureResources(ProcessOwner& owner) {
    for (auto& internal : owner.internals) {
        internal.reset();
    }
    owner.internalEnabledMask.store(0, std::memory_order_release);
    owner.producerRing.Clear();
    owner.logicRing.Clear();
    owner.identityResourcesReady.store(false, std::memory_order_release);
    owner.identityRing.Clear();
    owner.identityMirror.Clear();
    owner.identityStorageBytes = 0;
    owner.symbolResourcesReady.store(false, std::memory_order_release);
    owner.symbolRing.Clear();
    owner.symbolMirror.Clear();
    owner.symbolStorageBytes = 0;
    std::vector<std::uint8_t>().swap(owner.drainScratch);
    std::vector<std::uint8_t>().swap(owner.audioPackScratch);
    std::vector<float>().swap(owner.interleaveScratch);
    owner.captureResourcesReady = false;
}

void EnsureCaptureResources(ProcessOwner& owner) {
    if (owner.captureResourcesReady) {
        return;
    }
    const std::uint32_t slotBytes = PackedProducerSlotBytes();
    const auto slotCost = slotBytes + MonitorProducerRing::kSlotMetadataBytes;
    std::uint32_t producerSlots = 1;
    while (producerSlots * UINT64_C(2) * slotCost <= VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES)
        producerSlots *= 2;
    // Logic facts and logs use the same byte budget without reserving audio chunks.
    static_assert(sizeof(MonitorExecutionFact) <= PackedRecordBytes(sizeof(VnMonitorEventDetailV2)));
    const auto logicSlotBytes = PackedLogicProducerSlotBytes();
    const auto logicSlotCost = logicSlotBytes + MonitorProducerRing::kSlotMetadataBytes;
    std::uint32_t logicSlots = 1;
    while (logicSlots * UINT64_C(2) * logicSlotCost <= VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES)
        logicSlots *= 2;
    try {
        owner.producerRing.Init(producerSlots, slotBytes);
        if (owner.failCaptureResourcesForTests) {
            owner.failCaptureResourcesForTests = false;
            throw std::bad_alloc();
        }
        owner.logicRing.Init(logicSlots, logicSlotBytes);
        owner.drainScratch.assign(slotBytes, 0);
        owner.audioPackScratch.assign(slotBytes, 0);
        owner.interleaveScratch.assign(kInterleaveScratchFloats, 0.0f);
        owner.internals[kInternalRecorder] = std::make_unique<ConsumerQueue>();
        owner.captureResourcesReady = true;
    } catch (...) {
        ReleaseCaptureResources(owner);
        throw;
    }
}

bool StartDrain(ProcessOwner& owner) {
    if (owner.drainRunning.load(std::memory_order_acquire)) {
        return true;
    }
#if defined(__APPLE__)
    if (owner.drainWakeSemaphore == nullptr) {
        return false;
    }
#endif
    const bool hadResources = owner.captureResourcesReady;
    EnsureCaptureResources(owner);
    if (owner.drainPausedForTests) {
        return true;
    }
    owner.drainStop.store(false, std::memory_order_release);
    try {
        owner.drainThread = std::thread(DrainLoop, &owner);
    } catch (...) {
        owner.drainStop.store(true, std::memory_order_release);
        if (!hadResources) {
            ReleaseCaptureResources(owner);
        }
        return false;
    }
    owner.drainRunning.store(true, std::memory_order_release);
    return true;
}

bool PublishToRing(MonitorProducerRing& ring, const std::uint8_t* bytes,
                   std::uint32_t size, MonitorProducerOrigin origin = {}) {
    if (bytes == nullptr || size == 0) {
        return false;
    }
    ProcessOwner& owner = Owner();
#if !defined(HARKEN_MONITOR_HUB_ABSENT)
    if (!ring.TryPush(bytes, size, 0, origin, RecordingRawStreams(bytes, size, origin))) {
        const std::uint64_t sourceSequence =
            ReadPackedU64(bytes, kPackedSourceSequenceOffset);
        if (&ring == &owner.producerRing) {
            owner.producerDrop.Note(sourceSequence, size, VN_MONITOR_EVENT_DROP);
        }
        return false;
    }
    owner.publishedCount.fetch_add(1, std::memory_order_acq_rel);
    owner.WakeDrain();
    return true;
#else
    (void)owner;
    (void)ring;
    return false;
#endif
}

bool PublishPacked(const std::uint8_t* bytes, std::uint32_t size) {
    return PublishToRing(Owner().producerRing, bytes, size);
}

bool PublishLogicPacked(const std::uint8_t* bytes, std::uint32_t size) {
    return PublishToRing(Owner().logicRing, bytes, size);
}

std::uint64_t NextLogicSourceSequence() {
    return Owner().nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t CurrentExecuteFrame() {
    return Owner().executeFrame.load(std::memory_order_relaxed);
}

bool LogicThreadReady() {
    return RequireEngineAndLogicThread(false) == VN_OK;
}

void ResetLifecycleDedupeForTests() {
    ProcessOwner& owner = Owner();
    owner.lifecycleSeen = {};
    owner.lifecycleSeenNext = 0;
    owner.lateWorldGapSeen = {};
    owner.lateWorldGapNext = 0;
}

void SetCurrentWorldGenerationForTests(std::uint64_t world) {
    Owner().currentWorldGeneration.store(world, std::memory_order_release);
}

std::uint32_t LogicRingCapacityForTests() { return Owner().logicRing.Capacity(); }

bool FillLogicRingForTests() {
    return FillLogicRingLeavingFreeSlotsForTests(0);
}

bool FillLogicRingLeavingFreeSlotsForTests(std::uint32_t freeSlots) {
    ProcessOwner& owner = Owner();
    if (owner.logicRing.Capacity() < freeSlots) {
        return false;
    }
    std::uint8_t packed[kScratchBytes];
    while (owner.logicRing.FreeSlots() > freeSlots) {
        const std::uint64_t sourceSequence =
            owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t size =
            PackSyntheticTimeSeries(packed, sizeof(packed), sourceSequence);
        if (size == 0 || !PublishLogicPacked(packed, size)) {
            break;
        }
    }
    return owner.logicRing.FreeSlots() == freeSlots;
}

std::uint32_t LogicRingFreeSlotsForTests() { return Owner().logicRing.FreeSlots(); }

void ObserveDspGapForTests() { harken::ObserveMonitorDspGap(); }

VnResult RequireEngineAndLogicThread(bool pollPath) {
    ProcessOwner& owner = Owner();
    if (harken::detail::ActiveEngine() == nullptr || owner.engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (!pollPath && std::this_thread::get_id() != owner.logicThread) {
        return VN_MONITOR_WRONG_THREAD;
    }
    return VN_OK;
}

VnResult GateControlEntry() {
    return RequireEngineAndLogicThread(false);
}

Session* FindLive(ProcessOwner& owner, VnMonitorSessionHandle handle, bool allowRecording = false) {
    const std::uint32_t slot = HandleSlot(handle);
    const std::uint64_t generation = HandleGeneration(handle);
    if (slot >= owner.sessions.size()) {
        return nullptr;
    }
    Session& session = owner.sessions[slot];
    if (!session.live.load(std::memory_order_acquire) ||
        session.generation != generation || (session.recordingReceiver && !allowRecording)) {
        return nullptr;
    }
    return &session;
}

void RetireSessionLocked(ProcessOwner& owner, Session& session) {
    if (session.sealRequested) --owner.pendingIdentitySeals;
    if (session.eventCatalog) owner.eventObservers.fetch_sub(1, std::memory_order_release);
    if (session.executionFactVersion != 0) {
        if ((session.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) != 0)
            owner.executionObservers.fetch_sub(1, std::memory_order_release);
        if (!session.identityInterestReleased) {
            if ((session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0) --owner.identityObservers;
            if (WantsSymbols(session)) --owner.symbolObservers;
        }
    }
    owner.RetireIfPollingLocked(session);
    session.Reset();
    owner.observerCount.fetch_sub(1, std::memory_order_release);
}

void CleanupRecordingReceiverLocked(ProcessOwner& owner) {
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    auto& recorder = owner.recorder;
    if (!recorder.typed || recorder.receiver == 0) return;
    auto* session = FindLive(owner, recorder.receiver, true);
    if (!session) return;
    if (recorder.identityCleanupPending && !session->identityInterestReleased) {
        if ((session->acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0) --owner.identityObservers;
        if (WantsSymbols(*session)) --owner.symbolObservers;
        session->identityInterestReleased = true;
    }
    recorder.identityCleanupPending = false;
    if (recorder.state == VN_MONITOR_RECORDING_STATE_ACTIVE || recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING) return;
    RetireSessionLocked(owner, *session);
    recorder.receiver = 0;
}

void FillCapabilities(VnMonitorCapabilities* out, const ProcessOwner& owner) {
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->monitorImplemented = kImplementedApiMask;
    out->remoteEnabled = owner.endpoint.Enabled() ? 1u : 0u;
    out->securityProfile = owner.endpoint.profile;
    out->keepPlanAfterLeaseTimeout = 0;
    out->resolvedSessionsPerEngine = VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE;
    out->hardSessionsPerEngine = VN_MONITOR_HARD_SESSIONS_PER_ENGINE;
    out->resolvedProcessSessionSlots = VN_MONITOR_HARD_PROCESS_SESSION_SLOTS;
    out->hardProcessSessionSlots = VN_MONITOR_HARD_PROCESS_SESSION_SLOTS;
    out->resolvedProcessQueueBytes = VN_MONITOR_HARD_PROCESS_QUEUE_BYTES;
    out->hardProcessQueueBytes = VN_MONITOR_HARD_PROCESS_QUEUE_BYTES;
    out->resolvedRemoteClients = VN_MONITOR_DEFAULT_REMOTE_CLIENTS;
    out->hardRemoteClients = VN_MONITOR_HARD_REMOTE_CLIENTS;
    out->resolvedPlanSlots = VN_MONITOR_DEFAULT_PLAN_SLOTS;
    out->hardPlanSlots = VN_MONITOR_HARD_PLAN_SLOTS;
    out->dedicatedStopSlots = VN_MONITOR_DEDICATED_STOP_SLOTS;
    out->lufsMomentarySupported =
        core::LoudnessSampleRateSupported(owner.engine->config.mixRate.SampleRate()) ? 1u : 0u;
    out->lufsShortTermSupported = 0;
    out->lufsIntegratedSupported = 0;
    out->resolvedProducerRingBytes = VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES;
    out->hardProducerRingBytes = VN_MONITOR_HARD_PRODUCER_RING_BYTES;
    out->resolvedConsumerQueueBytes = VN_MONITOR_DEFAULT_CONSUMER_QUEUE_BYTES;
    out->hardConsumerQueueBytes = VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES;
    out->resolvedDerivedWorkItems = VN_MONITOR_DEFAULT_DERIVED_WORK_ITEMS;
    out->hardDerivedWorkItems = VN_MONITOR_HARD_DERIVED_WORK_ITEMS;
    out->resolvedRecorderQueueBytes = VN_MONITOR_DEFAULT_RECORDER_QUEUE_BYTES;
    out->hardRecorderQueueBytes = VN_MONITOR_HARD_RECORDER_QUEUE_BYTES;
    out->resolvedRemoteQueueBytes = VN_MONITOR_DEFAULT_REMOTE_QUEUE_BYTES;
    out->hardRemoteQueueBytes = VN_MONITOR_HARD_REMOTE_QUEUE_BYTES;
    out->resolvedCaptureByteRate = VN_MONITOR_DEFAULT_CAPTURE_BYTE_RATE;
    out->hardCaptureByteRate = VN_MONITOR_HARD_CAPTURE_BYTE_RATE;
    out->resolvedRawAudioByteRate = VN_MONITOR_DEFAULT_RAW_AUDIO_BYTE_RATE;
    out->hardRawAudioByteRate = VN_MONITOR_HARD_RAW_AUDIO_BYTE_RATE;
}

void ObserveEmitterChange(MonitorEmitterRegistry::Change change,
                           const MonitorEmitterOrigin& origin) noexcept {
    if (!ExecutionObservationEnabled() || !LogicThreadReady()) return;
    auto& engine = *Owner().engine;
    MonitorExecutionFact fact{};
    fact.engine = engine.monitorRequests.EngineInstance();
    fact.timestamp = LogTimestampNs();
    fact.subjectNamespace = VN_MONITOR_SUBJECT_EMITTER;
    fact.body.execution.identity.handle = origin.emitter;
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_SUBJECT);
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_TIMESTAMP);
    switch (change) {
    case MonitorEmitterRegistry::Change::Created: fact.kind = VN_MONITOR_FACT_EMITTER_CREATED; break;
    case MonitorEmitterRegistry::Change::BindingChanged: fact.kind = VN_MONITOR_FACT_EMITTER_BINDING_CHANGED; break;
    case MonitorEmitterRegistry::Change::Destroyed: fact.kind = VN_MONITOR_FACT_EMITTER_DESTROYED; break;
    }
    // A destroyed emitter's origin is a saved value. Never query its reused
    // slot for historical metadata; only resolve current binding by generation.
    const auto context = engine.emitters.Context(origin.emitter);
    CaptureFactEmitters(fact, origin,
        engine.monitorEmitters.CaptureBinding(origin, context.stateEmitter, context.spatialEmitter));
    PublishExecutionFact(fact);
}

VnResult OnEngineInit(Engine* engine) {
    ProcessOwner& owner = Owner();
    bool failAfterThread = false;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        owner.SweepRetiredLocked();
        if (owner.InitWouldExceedLocked()) {
            return VN_IN_USE;
        }
        owner.engine = engine;
        engine->monitorEmitters.SetChangeSink(ObserveEmitterChange);
        owner.logicThread = std::this_thread::get_id();
        owner.nextTimeSeriesSequence.store(1, std::memory_order_relaxed);
        owner.lifecycleSeen = {};
        owner.lifecycleSeenNext = 0;
        owner.nextStateSequence = 1;
        owner.nextDropSourceSequence = 1ull << 32;
        owner.producerDrop.Clear();
        owner.producerDrop.stage.store(VN_MONITOR_DROP_SOURCE_PRODUCER,
                                       std::memory_order_relaxed);
        owner.producerDrop.streamId.store(kSyntheticStreamId,
                                          std::memory_order_relaxed);
        owner.observerCount.store(0, std::memory_order_release);
        owner.eventObservers.store(0, std::memory_order_release);
        owner.executionObservers.store(0, std::memory_order_release);
        for (auto& producer : owner.executionProducers) producer.Reset();
        for (auto& completed : owner.ringProjectionCompleted) completed.store(0, std::memory_order_relaxed);
        for (auto& completed : owner.logProjectionCompleted) completed.store(0, std::memory_order_relaxed);
        owner.pendingRingTickets = owner.logTranscriptLast = {};
        owner.controlPublished = 0;
        owner.logicUnavailable = {};
        owner.recorderLogicPending.store(false, std::memory_order_relaxed);
        owner.identityProducer.Reset();
        owner.symbolProducer.Reset();
        owner.observationRevision = 1;
        owner.nextIdentitySequence = 1;
        owner.identityMutationDepth = 0;
        owner.identityObservers = 0;
        owner.symbolObservers = 0;
        owner.pendingIdentitySeals = 0;
        owner.identityRefreshes = 0;
        owner.dropIdentityRowForTests = -1;
        owner.lastEventFanoutSequence.store(0, std::memory_order_release);
        owner.internalEnabledMask.store(0, std::memory_order_release);
        owner.drainPausedForTests = false;
        owner.publishMutexHits.store(0, std::memory_order_relaxed);
        owner.drainHold.store(false, std::memory_order_relaxed);
        owner.drainEntered.store(0, std::memory_order_relaxed);
        owner.fanoutHold.store(false, std::memory_order_relaxed);
        owner.fanoutEntered.store(0, std::memory_order_relaxed);
        owner.publishedCount.store(0, std::memory_order_relaxed);
        owner.fanoutCompleted.store(0, std::memory_order_relaxed);
        owner.lastLogicFanoutSequence.store(0, std::memory_order_relaxed);
        for (Session& session : owner.sessions) {
            session.Reset();
        }
        owner.ResetPlanLocked();
        failAfterThread = owner.failInitAfterThread;
        owner.failInitAfterThread = false;
    }
    t_hostPrincipal = DefaultHostPrincipal();
    t_hostPrincipalOverride = false;
    if (failAfterThread) {
        StartDrain(owner);
        StopDrain(owner);
        std::lock_guard<std::mutex> lock(owner.mutex);
        owner.engine = nullptr;
        return VN_IN_USE;
    }
    return VN_OK;
}

void OnEngineTeardown(Engine* engine) {
    ProcessOwner& owner = Owner();
    StopDrain(owner);
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (owner.engine != engine && owner.engine != nullptr) {
        return;
    }
    for (Session& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire)) {
            continue;
        }
        owner.RetireIfPollingLocked(session);
        session.Reset();
    }
    owner.observerCount.store(0, std::memory_order_release);
    owner.eventObservers.store(0, std::memory_order_release);
    owner.executionObservers.store(0, std::memory_order_release);
    owner.identityObservers = 0;
    owner.symbolObservers = 0;
    owner.pendingIdentitySeals = 0;
    owner.identityMutationDepth = 0;
    if (engine != nullptr) engine->monitorEmitters.SetChangeSink(nullptr);
    ReleaseCaptureResources(owner);
    {
        std::lock_guard<std::mutex> lock(owner.recorder.mutex);
        AbandonIncompleteRecordingLocked(owner.recorder);
        std::vector<std::uint8_t>().swap(owner.recorder.packBuffer);
    }
    CloseBoundPipe(owner.endpoint);
    owner.endpoint = {};
    owner.engine = nullptr;
    owner.ResetPlanLocked();
    owner.SweepRetiredLocked();
    t_hostPrincipal = {};
    t_hostPrincipalOverride = false;
}

bool InitWouldExceedProcessCap() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return owner.InitWouldExceedLocked();
}

void SetPollStallForTests(bool stall) {
    Owner().pollStall.store(stall, std::memory_order_release);
}

std::uint32_t PollEnteredForTests() {
    return Owner().pollEntered.load(std::memory_order_acquire);
}

void ResetPollEnteredForTests() {
    Owner().pollEntered.store(0, std::memory_order_release);
}

VnResult GetCapabilities(VnMonitorCapabilities* outCapabilities) {
    if (outCapabilities == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    if (!ValidStructSize(outCapabilities->structSize,
                         sizeof(VnMonitorCapabilities))) {
        return VN_INVALID_ARG;
    }
    if (!ReservedZero(outCapabilities->reserved0) ||
        !ReservedZero(outCapabilities->reserved1) ||
        !ReservedZero(outCapabilities->reserved2)) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.SweepRetiredLocked();
    FillCapabilities(outCapabilities, owner);
    return VN_OK;
}

void FillTapDescriptor(VnMonitorTapDescriptor* tap, std::uint32_t tapKind,
                       ProcessOwner& owner) {
    std::memset(tap, 0, sizeof(*tap));
    tap->structSize = sizeof(*tap);
    tap->tapKind = tapKind;
    tap->tapRegistryGeneration = owner.tapRegistryGeneration;
    tap->tapDescriptorId = tapKind;
    tap->tapDescriptorGeneration = 1;
    tap->selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    if (tapKind == VN_MONITOR_TAP_DEVICE_OUTPUT) {
        tap->ownerKind = VN_MONITOR_ENTITY_NONE;
        tap->availabilityState = 1;
        tap->reason = VN_MONITOR_UNSUPPORTED;
        return;
    }
    if (tapKind == VN_MONITOR_TAP_VOICE_PRE_CHAIN ||
        tapKind == VN_MONITOR_TAP_VOICE_POST_CHAIN) {
        tap->ownerKind = VN_MONITOR_ENTITY_VOICE;
        return;
    }
    if (tapKind == VN_MONITOR_TAP_EFFECT_SLOT_INPUT ||
        tapKind == VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT) {
        tap->ownerKind = VN_MONITOR_ENTITY_EFFECT;
        return;
    }
    tap->ownerKind = VN_MONITOR_ENTITY_BUS;
    if (tapKind == VN_MONITOR_TAP_MASTER_OUTPUT) {
        tap->ownerStableId = VN_BUS_MASTER;
        tap->ownerInstanceHandle = 1;
        if (owner.engine != nullptr) {
            const std::uint32_t busIndex =
                owner.engine->buses.FindIndex(VN_BUS_MASTER);
            if (busIndex != kInvalidBusIndex) {
                tap->ownerGeneration = owner.engine->buses.GenerationAt(busIndex);
            }
        }
    }
}

VnResult CaptureRegistry(uint32_t kind, void* out, uint32_t capacityBytes,
                         uint32_t* requiredBytes, uint64_t* revision) {
    if (requiredBytes == nullptr || revision == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    if (kind != VN_MONITOR_REGISTRY_TAP) {
        return VN_MONITOR_UNSUPPORTED;
    }
    static constexpr std::uint32_t kKinds[] = {
        VN_MONITOR_TAP_VOICE_PRE_CHAIN, VN_MONITOR_TAP_VOICE_POST_CHAIN,
        VN_MONITOR_TAP_EFFECT_SLOT_INPUT, VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT,
        VN_MONITOR_TAP_BUS_POST_EFFECTS, VN_MONITOR_TAP_BUS_PRE_FADER,
        VN_MONITOR_TAP_BUS_POST_FADER, VN_MONITOR_TAP_MASTER_OUTPUT,
        VN_MONITOR_TAP_DEVICE_OUTPUT};
    const std::uint32_t bytes = static_cast<std::uint32_t>(sizeof(VnMonitorTapDescriptor) *
                                                           (sizeof(kKinds) / sizeof(kKinds[0])));
    *requiredBytes = bytes;
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    *revision = owner.tapRegistryGeneration;
    if (out == nullptr || capacityBytes < bytes) {
        return VN_MONITOR_BUFFER_TOO_SMALL;
    }
    auto* taps = static_cast<VnMonitorTapDescriptor*>(out);
    for (std::uint32_t i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); ++i) {
        FillTapDescriptor(&taps[i], kKinds[i], owner);
    }
    return VN_OK;
}

VnResult ReadExecutionVersion(const VnMonitorSessionDesc* desc, std::uint32_t& version) {
    version = 0;
    if (desc == nullptr || (desc->structSize != sizeof(VnMonitorSessionDesc) &&
        desc->structSize != sizeof(VnMonitorSessionDescV2))) return VN_INVALID_ARG;
    if (desc->reserved0 != 0) return VN_INVALID_ARG;
    if (desc->structSize == sizeof(VnMonitorSessionDescV2)) {
        std::uint32_t extension[2]{};
        std::memcpy(extension, reinterpret_cast<const std::uint8_t*>(desc) +
            sizeof(VnMonitorSessionDesc), sizeof(extension));
        if (extension[1] != 0) return VN_INVALID_ARG;
        if (extension[0] > 2) return VN_MONITOR_UNSUPPORTED;
        version = extension[0];
    }
    return VN_OK;
}

bool IdentitySafeBoundary(const ProcessOwner& owner) {
    return LogicThreadReady() && owner.identityMutationDepth == 0 && owner.engine->callbackDispatchDepth == 0;
}

std::array<std::uint64_t, 2> SymbolUnavailableNow(const ProcessOwner& owner) {
    return {owner.symbolProducer.Unavailable(), owner.symbolMirror.Unavailable()};
}

std::uint64_t SymbolUnavailable(const ProcessOwner& owner, const Session& session) {
    if (!WantsSymbols(session)) return 0;
    const auto now = session.eventStoppedNs == 0 ? SymbolUnavailableNow(owner) : session.symbolUnavailableEnd;
    std::uint64_t total = 0;
    for (unsigned i = 0; i != now.size(); ++i) {
        const auto count = now[i] - session.symbolUnavailableStart[i];
        total = count > UINT64_MAX - total ? UINT64_MAX : total + count;
    }
    return total;
}

bool CaptureSymbolSlot(const ProcessOwner& owner, std::size_t slot, MonitorHostSymbolValue& value) {
    return owner.engine->monitorEmitters.CaptureDescriptionAt(static_cast<std::uint32_t>(slot),
        value.origin, value.description);
}

// Called with owner.mutex held, after a complete logical-boundary refresh.
VnResult FreezeIdentityCut(ProcessOwner& owner, Session& session, std::uint32_t hint) {
    auto& stage = session.identity;
    if (stage.active) return VN_BACKPRESSURE;
    {
        std::lock_guard<std::mutex> queueLock(session.queue->mutex);
        if (!session.queue->deferredSymbols.empty()) return VN_BACKPRESSURE;
    }
    if (owner.nextIdentitySequence == 0) return VN_MONITOR_UNSUPPORTED;
    const auto domain = stage.rows.size() + UINT64_C(3) * stage.symbols.size();
    const auto capacity = hint == 0 ? domain : std::min<std::uint64_t>(hint, domain);
    stage.begin = {};
    stage.begin.structSize = sizeof(stage.begin);
    stage.begin.stateCutId = owner.nextIdentitySequence;
    stage.begin.identityRevision = owner.observationRevision;
    stage.begin.deltaProducerId = kIdentityDeltaProducer;
    stage.begin.deltaSourceSequence = owner.identityProducer.Published();
    stage.begin.capacityLimit = domain;
    stage.sourceComplete = owner.identityMirror.Copy(
        std::span<MonitorExecutionFact>(stage.rows.data(), std::min<std::uint64_t>(capacity, stage.rows.size())),
        stage.identityCount, stage.begin.namespaceTrueCount);
    stage.count = stage.identityCount;
    stage.sourceComplete &= owner.identityProducer.Unavailable() == 0;
    if (WantsSymbols(session)) {
        stage.begin.symbolProducerId = kHostSymbolProducer;
        stage.begin.symbolSourceSequence = owner.symbolProducer.Published();
        const auto remaining = capacity - stage.count;
        std::uint32_t hosts = 0;
        stage.sourceComplete &= owner.symbolMirror.Copy(
            std::span<MonitorHostSymbolValue>(stage.symbols.data(),
                std::min<std::uint64_t>(stage.symbols.size(), (remaining + 2) / 3)),
            hosts, stage.begin.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL]);
        const auto fields = std::min<std::uint64_t>(remaining, UINT64_C(3) * hosts);
        stage.count += static_cast<std::uint32_t>(fields);
        stage.sourceComplete &= fields == stage.begin.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL] &&
            owner.symbolProducer.Unavailable() == 0;
        session.symbolStart = stage.begin.symbolSourceSequence;
        session.symbolAvailability = VN_MONITOR_OBSERVATION_PENDING;
    }
    if (static_cast<std::uint64_t>(stage.count) + 1 > UINT64_MAX - owner.nextIdentitySequence)
        return VN_MONITOR_UNSUPPORTED;
    stage.firstSequence = owner.nextIdentitySequence;
    const auto end = stage.firstSequence + stage.count + 1;
    owner.nextIdentitySequence = end == UINT64_MAX ? 0 : end + 1;
    stage.engine = owner.engine->monitorRequests.EngineInstance();
    stage.cursor = 0;
    stage.checksum = kIdentityChecksumSeed;
    std::fill(std::begin(stage.emitted), std::end(stage.emitted), 0);
    stage.delivered = {};
    stage.active = true;
    {
        std::lock_guard<std::mutex> queueLock(session.queue->mutex);
        session.queue->symbolBaselineActive = WantsSymbols(session);
    }
    stage.requested = false;
    session.identityStart = stage.begin.deltaSourceSequence;
    session.cut = {};
    session.cut.id = stage.begin.stateCutId;
    session.cut.beginSourceSequence = stage.firstSequence;
    session.cut.endSourceSequence = end;
    session.baselineAvailability = VN_MONITOR_OBSERVATION_PENDING;
    owner.WakeDrain();
    return VN_OK;
}

void SealAtIdentityBoundary(ProcessOwner& owner, Session& session) {
    if (session.sealRequested) --owner.pendingIdentitySeals;
    session.sealRequested = false;
    if (session.eventStoppedNs != 0) return;
    if (session.executionFactVersion != 0 && (session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0) {
        session.identityEnd = owner.identityProducer.Published();
        --owner.identityObservers;
    }
    if (WantsSymbols(session)) {
        session.symbolEnd = owner.symbolProducer.Published();
        session.symbolUnavailableEnd = SymbolUnavailableNow(owner);
        --owner.symbolObservers;
    }
    session.eventStoppedNs = LogTimestampNs();
    session.identityInterestReleased = true;
    session.sealedAudioLogs = owner.engine->logRing.PublishedCount();
    session.sealedLogicLogs = owner.engine->logicLogRing.PublishedCount();
    for (unsigned i = 0; i != owner.executionProducers.size(); ++i)
        session.executionEnds[i] = owner.executionProducers[i].Published();
    if (session.recordingReceiver) {
        session.ringEnds[0] = std::max(session.ringStarts[0], owner.logicRing.PublishedForStreams(session.acceptedStreamKindMask));
        session.ringEnds[1] = std::max(session.ringStarts[1], owner.producerRing.PublishedForStreams(session.acceptedStreamKindMask));
        session.rawDropsAtEnd = {owner.logicRing.DroppedForStreams(session.acceptedStreamKindMask),
            owner.producerRing.DroppedForStreams(session.acceptedStreamKindMask)};
        session.unavailableAtEnd = owner.logicUnavailable;
        session.unavailableAtEnd[0] = owner.executionProducers[0].Unavailable();
        session.unavailableAtEnd[1] = owner.executionProducers[1].Unavailable();
        session.logBudgetDropsAtEnd = owner.engine->logRing.QuantumBudgetDropCount();
        session.controlEnd = owner.controlPublished;
    }
}

void StopOwnedRecordingLocked(ProcessOwner& owner, VnMonitorSessionHandle handle,
    std::uint64_t lease, std::uint32_t reason) {
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    auto& recorder = owner.recorder;
    if (!recorder.typed || (recorder.state != VN_MONITOR_RECORDING_STATE_ACTIVE &&
        recorder.state != VN_MONITOR_RECORDING_STATE_STOPPING) ||
        recorder.descriptor.owner != handle || recorder.descriptor.leaseGeneration != lease) return;
    auto* session = FindLive(owner, recorder.receiver, true);
    if (!session) return;
    const bool alreadyStopping = recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING;
    recorder.state = VN_MONITOR_RECORDING_STATE_STOPPING;
    if (!alreadyStopping) recorder.stopReason = reason;
    if (std::this_thread::get_id() == owner.logicThread) {
        if (alreadyStopping) return;
        if (IdentitySafeBoundary(owner)) SealAtIdentityBoundary(owner, *session);
        else {
            if (!session->sealRequested) ++owner.pendingIdentitySeals;
            session->sealRequested = true;
        }
    } else {
        // No logic scan or interest mutation is permitted on this thread.
        recorder.forceIncomplete = recorder.identityCleanupPending = true;
        if (session->eventStoppedNs != 0) {
            owner.recorderLogicPending.store(true, std::memory_order_release);
            owner.WakeDrain();
            return;
        }
        session->eventStoppedNs = LogTimestampNs();
        session->sealedAudioLogs = owner.engine->logRing.PublishedCount();
        session->sealedLogicLogs = owner.engine->logicLogRing.PublishedCount();
        for (unsigned i = 0; i != 2; ++i) session->executionEnds[i] = owner.executionProducers[i].Published();
        session->identityEnd = owner.identityProducer.Published();
        session->symbolEnd = owner.symbolProducer.Published();
        session->ringEnds = {std::max(session->ringStarts[0], owner.logicRing.PublishedForStreams(session->acceptedStreamKindMask)),
            std::max(session->ringStarts[1], owner.producerRing.PublishedForStreams(session->acceptedStreamKindMask))};
        session->rawDropsAtEnd = {owner.logicRing.DroppedForStreams(session->acceptedStreamKindMask),
            owner.producerRing.DroppedForStreams(session->acceptedStreamKindMask)};
        session->unavailableAtEnd = owner.logicUnavailable;
        session->unavailableAtEnd[0] = owner.executionProducers[0].Unavailable();
        session->unavailableAtEnd[1] = owner.executionProducers[1].Unavailable();
        session->logBudgetDropsAtEnd = owner.engine->logRing.QuantumBudgetDropCount();
        session->controlEnd = owner.controlPublished;
        if (session->identity.requested) {
            session->identity.requested = false;
            session->baselineAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
        }
        owner.recorderLogicPending.store(true, std::memory_order_release);
    }
    owner.WakeDrain();
}

void RefreshIdentityBoundary() noexcept {
    auto& owner = Owner();
    if (owner.recorderLogicPending.load(std::memory_order_acquire) && IdentitySafeBoundary(owner)) {
        std::lock_guard<std::mutex> lock(owner.mutex);
        CleanupRecordingReceiverLocked(owner);
        owner.recorderLogicPending.store(false, std::memory_order_release);
    }
    if ((owner.identityObservers == 0 && owner.symbolObservers == 0 && owner.pendingIdentitySeals == 0) ||
        !IdentitySafeBoundary(owner)) return;
    const auto beforeIdentity = owner.identityMirror.Revision();
    const auto beforeSymbols = owner.symbolMirror.Revision();
    const bool canAdvance = owner.observationRevision != UINT64_MAX;
    const auto revision = canAdvance ? owner.observationRevision + 1 : owner.observationRevision;
    if (owner.identityObservers != 0) {
        ++owner.identityRefreshes;
        owner.identityMirror.Refresh(
        [&](std::size_t slot, MonitorExecutionFact& fact) { return CaptureIdentitySlot(*owner.engine, slot, fact); },
        [&](const MonitorExecutionFact& fact, std::uint64_t) {
            MonitorIdentityDelta delta{};
            delta.engine = fact.engine;
            delta.revision = revision;
            delta.fact = fact;
            owner.identityProducer.Publish(owner.identityRing, delta, fact.engine);
        }, canAdvance);
    }
    if (owner.symbolObservers != 0) {
        owner.symbolMirror.Refresh(
            [&](std::size_t slot, MonitorHostSymbolValue& value) { return CaptureSymbolSlot(owner, slot, value); },
            [&](const MonitorHostSymbolValue& value, unsigned kind) {
                for (unsigned field = 1; field <= 3; ++field) {
                    const auto source = HostSymbolField(value, field, kind, revision);
                    owner.symbolProducer.Publish(owner.symbolRing, source, source.engine);
                }
            }, canAdvance);
    }
    if (owner.identityMirror.Revision() != beforeIdentity || owner.symbolMirror.Revision() != beforeSymbols)
        owner.observationRevision = revision;
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.logicUnavailable = {owner.executionProducers[0].Unavailable(), owner.executionProducers[1].Unavailable(),
        owner.identityProducer.Unavailable() + owner.identityMirror.Unavailable(),
        owner.symbolProducer.Unavailable() + owner.symbolMirror.Unavailable()};
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || session.executionFactVersion == 0 ||
            session.eventStoppedNs != 0) continue;
        if ((session.acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0) {
            if (session.identity.requested && FreezeIdentityCut(owner, session, 0) != VN_OK) {
                session.baselineAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
                session.identity.requested = false;
                session.cut.settled = true;
                if (WantsSymbols(session)) MarkSymbolIncomplete(session);
            }
            if (owner.identityProducer.Unavailable() != 0 || owner.identityMirror.Unavailable() != 0) {
                session.baselineAvailability = VN_MONITOR_OBSERVATION_INCOMPLETE;
                if (session.cut.settled && session.cut.result == VN_MONITOR_STATE_CUT_COMPLETE)
                    session.cut.result = VN_MONITOR_STATE_CUT_TRUNCATED;
            }
        }
        if (WantsSymbols(session) && SymbolUnavailable(owner, session) != 0) MarkSymbolIncomplete(session);
    }
    for (auto& session : owner.sessions)
        if (session.live.load(std::memory_order_acquire) && session.sealRequested)
            SealAtIdentityBoundary(owner, session);
    owner.WakeDrain();
}

IdentityMutationScope::IdentityMutationScope() noexcept {
    auto& owner = Owner();
    if (!LogicThreadReady()) return;
    engine_ = owner.engine->monitorRequests.EngineInstance();
    ++owner.identityMutationDepth;
}

IdentityMutationScope::~IdentityMutationScope() {
    auto& owner = Owner();
    // Never dereference the old engine: teardown can occur within an Update.
    if (!LogicThreadReady() || owner.engine->monitorRequests.EngineInstance() != engine_ ||
        owner.identityMutationDepth == 0) return;
    --owner.identityMutationDepth;
    RefreshIdentityBoundary();
}

VnResult OpenSessionWithPermissions(const VnMonitorSessionDesc* desc,
                                    bool remote,
                                    std::uint32_t remoteGrant,
                                    std::uint64_t principalHash,
                                    VnMonitorSessionHandle* outHandle,
                                    bool eventCatalog = false, bool recordingReceiver = false) {
    if (desc == nullptr || outHandle == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    std::uint32_t executionVersion = 0;
    const auto versionResult = ReadExecutionVersion(desc, executionVersion);
    if (versionResult != VN_OK) return versionResult;
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    if (executionVersion != 0) return VN_MONITOR_UNSUPPORTED;
#endif
    if (executionVersion != 0 && (desc->requestSymbols > 1 || desc->requestRawAudio > 1 ||
        (eventCatalog && ((desc->requestedPermissionMask & VN_MONITOR_PERM_OBSERVE_EVENTS) == 0 ||
            (desc->requestedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0)))) return VN_INVALID_ARG;
    const std::uint64_t requestedQueue =
        desc->consumerQueueBytes == 0 ? VN_MONITOR_DEFAULT_CONSUMER_QUEUE_BYTES
                                      : desc->consumerQueueBytes;
    if (requestedQueue > VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES) {
        return VN_MONITOR_BUDGET_EXCEEDED;
    }

    const HostPrincipal principal = CurrentHostPrincipal();
    if (!principal.authenticated || principal.observeMask == 0) {
        return VN_MONITOR_PERMISSION_DENIED;
    }
    std::uint32_t allowedStreams = kObserveStreamMask;
    if (remote) {
        allowedStreams = 0;
        if (remoteGrant & VN_MONITOR_PERM_OBSERVE_STATE) {
            allowedStreams |= VN_MONITOR_STREAM_STATE;
        }
        if (remoteGrant & VN_MONITOR_PERM_OBSERVE_EVENTS) {
            allowedStreams |= VN_MONITOR_STREAM_EVENT;
        }
        if (remoteGrant & VN_MONITOR_PERM_OBSERVE_MEASUREMENTS) {
            allowedStreams |= VN_MONITOR_STREAM_TIME_SERIES;
        }
        if (remoteGrant & VN_MONITOR_PERM_RAW_AUDIO) {
            allowedStreams |= VN_MONITOR_STREAM_OPTIONAL_AUDIO;
        }
    }
    if (executionVersion != 0) {
        if ((desc->requestedPermissionMask & VN_MONITOR_PERM_OBSERVE_EVENTS) == 0)
            allowedStreams &= ~VN_MONITOR_STREAM_EVENT;
        if ((desc->requestedPermissionMask & VN_MONITOR_PERM_OBSERVE_STATE) == 0)
            allowedStreams &= ~VN_MONITOR_STREAM_STATE;
        if ((desc->requestedPermissionMask & VN_MONITOR_PERM_OBSERVE_MEASUREMENTS) == 0)
            allowedStreams &= ~VN_MONITOR_STREAM_TIME_SERIES;
        if ((desc->requestedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO) == 0)
            allowedStreams &= ~VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    }
    const std::uint32_t acceptedMask =
        desc->requestedStreamKindMask & principal.observeMask & allowedStreams;
    if ((desc->requestedStreamKindMask & kObserveStreamMask) != 0 &&
        acceptedMask == 0) {
        return VN_MONITOR_PERMISSION_DENIED;
    }

    if (eventCatalog && executionVersion != 0 && (acceptedMask & VN_MONITOR_STREAM_EVENT) == 0)
        return VN_MONITOR_PERMISSION_DENIED;

    ProcessOwner& owner = Owner();
    std::unique_lock<std::mutex> lock(owner.mutex);
    owner.SweepRetiredLocked();
    if (owner.LiveSlotsLocked() >= VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE) {
        return VN_MONITOR_SESSION_CAPACITY;
    }
    const bool identityRequested = executionVersion != 0 && (acceptedMask & VN_MONITOR_STREAM_STATE) != 0;
    const auto identityCapacity = identityRequested ? IdentitySlotCapacity(*owner.engine) : 0;
    const bool symbolsRequested = executionVersion != 0 && desc->requestSymbols != 0 &&
        (desc->requestedPermissionMask & VN_MONITOR_PERM_SYMBOLS) != 0 &&
        (acceptedMask & (VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT)) != 0 &&
        (!remote || (remoteGrant & VN_MONITOR_PERM_SYMBOLS) != 0);
    const auto symbolCapacity = symbolsRequested ? owner.engine->monitorEmitters.Capacity() : 0;
    const auto symbolStagingCapacity = identityRequested ? symbolCapacity : 0;
    if (identityCapacity + UINT64_C(3) * symbolStagingCapacity > UINT32_MAX - 2)
        return VN_MONITOR_BUDGET_EXCEEDED;
    const auto identityStagingBytes = identityCapacity * sizeof(MonitorExecutionFact);
    const auto stagingBytes = identityStagingBytes + symbolStagingCapacity * sizeof(MonitorHostSymbolValue);
    constexpr auto identitySlotBytes = sizeof(MonitorIdentityDelta) + MonitorProducerRing::kSlotMetadataBytes;
    std::uint32_t identitySlots = 1;
    while (identitySlots * UINT64_C(2) * identitySlotBytes <= VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES)
        identitySlots *= 2;
    const auto newIdentityBytes = identityRequested && !owner.identityResourcesReady.load(std::memory_order_acquire)
        ? identityStagingBytes + identitySlots * identitySlotBytes : 0;
    constexpr auto symbolSlotBytes = sizeof(MonitorHostSymbolSource) + MonitorProducerRing::kSlotMetadataBytes;
    std::uint32_t symbolSlots = 1;
    while (symbolSlots * UINT64_C(2) * symbolSlotBytes <= VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES)
        symbolSlots *= 2;
    const auto newSymbolBytes = symbolsRequested && !owner.symbolResourcesReady.load(std::memory_order_acquire)
        ? symbolCapacity * sizeof(MonitorHostSymbolValue) + symbolSlots * symbolSlotBytes : 0;
    if (owner.WouldExceedLocked(1, requestedQueue + stagingBytes + newIdentityBytes + newSymbolBytes)) {
        return VN_MONITOR_SESSION_CAPACITY;
    }
    Session* slot = nullptr;
    std::uint32_t slotIndex = 0;
    for (std::uint32_t i = 0; i < owner.sessions.size(); ++i) {
        if (!owner.sessions[i].live.load(std::memory_order_acquire)) {
            slot = &owner.sessions[i];
            slotIndex = i;
            break;
        }
    }
    if (slot == nullptr) {
        return VN_MONITOR_SESSION_CAPACITY;
    }
    std::unique_ptr<ConsumerQueue> newQueue;
    try {
        if (!slot->queue) {
            newQueue = std::make_unique<ConsumerQueue>();
        }
        if (identityRequested) {
            slot->identity.rows.assign(identityCapacity, {});
            if (!owner.identityResourcesReady.load(std::memory_order_acquire)) {
                owner.identityMirror.Reset(identityCapacity);
                owner.identityRing.Init(identitySlots, sizeof(MonitorIdentityDelta));
                owner.identityStorageBytes = identityStagingBytes + identitySlots * identitySlotBytes;
                owner.identityResourcesReady.store(true, std::memory_order_release);
            }
        }
        if (symbolsRequested) {
            if (identityRequested) slot->identity.symbols.assign(symbolStagingCapacity, {});
            if (!owner.symbolResourcesReady.load(std::memory_order_acquire)) {
                owner.symbolMirror.Reset(symbolCapacity);
                owner.symbolRing.Init(symbolSlots, sizeof(MonitorHostSymbolSource));
                owner.symbolStorageBytes = newSymbolBytes;
                owner.symbolResourcesReady.store(true, std::memory_order_release);
            }
            if (owner.symbolObservers == 0)
                owner.symbolMirror.Seed([&](std::size_t index, MonitorHostSymbolValue& value) {
                    return CaptureSymbolSlot(owner, index, value);
                });
        }
#if !defined(HARKEN_MONITOR_HUB_ABSENT)
        if (!StartDrain(owner)) {
            slot->identity = {};
            return VN_IN_USE;
        }
#endif
    } catch (const std::bad_alloc&) {
        slot->identity = {};
        if (!owner.identityResourcesReady.load(std::memory_order_acquire)) {
            owner.identityMirror.Clear();
            owner.identityRing.Clear();
        }
        if (!owner.symbolResourcesReady.load(std::memory_order_acquire)) {
            owner.symbolMirror.Clear();
            owner.symbolRing.Clear();
        }
        return VN_MONITOR_BUDGET_EXCEEDED;
    }
    slot->generation = owner.nextGeneration++;
    slot->epoch = owner.nextEpoch++;
    slot->requestedStreamKindMask.store(desc->requestedStreamKindMask,
                                        std::memory_order_relaxed);
    slot->requestedPermissionMask = desc->requestedPermissionMask;
    slot->acceptedStreamKindMask = acceptedMask;
    const std::uint32_t knownPerm =
        kObservePermissionMask | kCaptureControlPermission | kRecordPermission |
        VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_SYMBOLS;
    const std::uint32_t requestedPerm = desc->requestedPermissionMask;
    std::uint32_t acceptedPerm = 0;
    std::uint32_t unsupportedPerm = 0;
    if (remote) {
        acceptedPerm = requestedPerm & remoteGrant & knownPerm;
        unsupportedPerm = requestedPerm & ~knownPerm;
    } else {
        acceptedPerm = (requestedPerm & kObservePermissionMask) |
                       (requestedPerm & kCaptureControlPermission) |
                       (requestedPerm & kRecordPermission) |
                       (executionVersion != 0 ? requestedPerm & (VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_SYMBOLS) : 0);
        unsupportedPerm = executionVersion != 0 ? 0u :
            requestedPerm & (VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_SYMBOLS);
    }
    if (executionVersion != 0) {
        if ((acceptedMask & VN_MONITOR_STREAM_STATE) == 0) acceptedPerm &= ~VN_MONITOR_PERM_OBSERVE_STATE;
        if ((acceptedMask & VN_MONITOR_STREAM_EVENT) == 0) acceptedPerm &= ~VN_MONITOR_PERM_OBSERVE_EVENTS;
        if ((acceptedMask & VN_MONITOR_STREAM_TIME_SERIES) == 0) acceptedPerm &= ~VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
        if ((acceptedMask & VN_MONITOR_STREAM_OPTIONAL_AUDIO) == 0) acceptedPerm &= ~VN_MONITOR_PERM_RAW_AUDIO;
        if ((acceptedMask & (VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT)) == 0)
            acceptedPerm &= ~VN_MONITOR_PERM_SYMBOLS;
    }
    slot->executionFactVersion = executionVersion;
    slot->supportedFactKinds = {};
    if (executionVersion != 0) {
        slot->supportedFactKinds[0] = (UINT64_C(1) << VN_MONITOR_FACT_REQUEST_SUBMITTED) |
            (UINT64_C(1) << VN_MONITOR_FACT_REQUEST_RETURNED) |
            (UINT64_C(1) << VN_MONITOR_FACT_EMITTER_CREATED) |
            (UINT64_C(1) << VN_MONITOR_FACT_EMITTER_DESTROYED) |
            (UINT64_C(1) << VN_MONITOR_FACT_EMITTER_BINDING_CHANGED) |
            (UINT64_C(1) << VN_MONITOR_FACT_IDENTITY_UPSERT) |
            (UINT64_C(1) << VN_MONITOR_FACT_IDENTITY_TOMBSTONE);
        for (unsigned phase = 1; phase <= static_cast<unsigned>(MonitorLifecyclePhase::PlayableStarted); ++phase) {
            const auto kind = LifecycleFactKind(static_cast<MonitorLifecyclePhase>(phase));
            if (kind != 0) slot->supportedFactKinds[0] |= UINT64_C(1) << kind;
        }
    }
    if (executionVersion >= 2) {
        slot->supportedFactKinds[0] |= (UINT64_C(1) << VN_MONITOR_FACT_ACTION_TERMINAL) |
            (UINT64_C(1) << VN_MONITOR_FACT_STEP_EXECUTED) | (UINT64_C(1) << VN_MONITOR_FACT_VARIABLE_RESULT);
    }
    slot->baselineAvailability = executionVersion != 0 &&
        (desc->requestedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0
            ? ((acceptedMask & VN_MONITOR_STREAM_STATE) != 0
                ? VN_MONITOR_OBSERVATION_PENDING : VN_MONITOR_OBSERVATION_DENIED)
            : VN_MONITOR_OBSERVATION_NOT_REQUESTED;
    slot->symbolAvailability = executionVersion != 0 && desc->requestSymbols != 0
        ? (symbolsRequested ? (identityRequested ? VN_MONITOR_OBSERVATION_PENDING : VN_MONITOR_OBSERVATION_AVAILABLE)
            : VN_MONITOR_OBSERVATION_DENIED) : VN_MONITOR_OBSERVATION_NOT_REQUESTED;
    slot->acceptedPermissionMask = acceptedPerm;
    slot->unsupportedPermissionMask = unsupportedPerm;
    slot->deniedPermissionMask = requestedPerm & ~(acceptedPerm | unsupportedPerm);
    slot->requestRawAudio = desc->requestRawAudio;
    slot->requestSymbols = desc->requestSymbols;
    const bool rawPerm = (acceptedPerm & VN_MONITOR_PERM_RAW_AUDIO) != 0;
    const bool symbolsPerm = (acceptedPerm & VN_MONITOR_PERM_SYMBOLS) != 0;
    slot->rawAudioDecision = desc->requestRawAudio == 0 ? 0u : (rawPerm ? 2u : 3u);
    slot->symbolsDecision =
        desc->requestSymbols == 0 ? 0u : (symbolsPerm ? 2u : 3u);
    slot->principalHash = principalHash;
    slot->consumerQueueBytes = requestedQueue;
    slot->firstExpectedTimeSeriesSequence =
        owner.nextTimeSeriesSequence.load(std::memory_order_relaxed);
    slot->firstExpectedStateSequence = owner.nextStateSequence;
    slot->pollInFlight.store(0, std::memory_order_relaxed);
    slot->resyncRequired.store(false, std::memory_order_relaxed);
    slot->streamTerminated.store(false, std::memory_order_relaxed);
    slot->cut = {};
    slot->identity.requested = identityRequested;
    slot->unavailableStarts = owner.logicUnavailable;
    slot->unavailableStarts[0] = owner.executionProducers[0].Unavailable();
    slot->unavailableStarts[1] = owner.executionProducers[1].Unavailable();
    slot->identityStart = owner.identityProducer.Published();
    slot->identityEnd = 0;
    slot->identityDrops = {};
    slot->identityDrops.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    slot->identityDrops.streamId = kIdentityDeltaStream;
    slot->baselineDrops = {};
    slot->baselineDrops.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    slot->baselineDrops.streamId = kIdentityBaselineStream;
    slot->symbolUnavailableStart = SymbolUnavailableNow(owner);
    slot->symbolStart = owner.symbolProducer.Published();
    slot->symbolEnd = 0;
    slot->symbolUnavailableEnd = {};
    slot->symbolDrops = {};
    slot->symbolDrops.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    slot->symbolDrops.streamId = kHostSymbolStream;
    slot->sealRequested = false;
    slot->eventCatalog = eventCatalog;
    slot->recordingReceiver = recordingReceiver;
    slot->identityInterestReleased = false;
    // Loss bounds enclose the publication window: losses first on open, last on seal.
    slot->rawDropStarts = {owner.logicRing.DroppedForStreams(acceptedMask), owner.producerRing.DroppedForStreams(acceptedMask)};
    slot->ringStarts = {owner.logicRing.Published(), owner.producerRing.Published()};
    slot->ringEnds = {};
    slot->nativeLogDrops = {owner.engine->logRing.QuantumBudgetDropCount(), 0};
    slot->logStarts = {owner.engine->logRing.PublishedCount(), owner.engine->logicLogRing.PublishedCount()};
    slot->logFiltered = slot->logLost = slot->logDownstreamEnds = {};
    slot->controlStart = owner.controlPublished;
    slot->controlEnd = slot->controlDrops = 0;
    slot->eventStartedNs = eventCatalog || recordingReceiver ? LogTimestampNs() : 0;
    slot->eventStoppedNs = 0;
    slot->lastEventSequence = 0;
    slot->eventSourceDrops = 0;
    slot->eventDrained = false;
    slot->finalSourceDrops = 0;
    for (unsigned i = 0; i != owner.executionProducers.size(); ++i) {
        slot->executionStarts[i] = owner.executionProducers[i].Published();
        slot->executionEnds[i] = 0;
        slot->executionSourceDrops[i] = {};
        slot->executionSourceDrops[i].stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
        slot->executionSourceDrops[i].streamId = i == 0 ? kExecutionLogicStream : kExecutionAudioStream;
    }
    slot->logDropBaseline = owner.engine->logRing.DroppedCount() +
        owner.engine->logicLogRing.DroppedCount() +
        owner.engine->logRing.QuantumBudgetDropCount();
    if (newQueue) {
        slot->queue = std::move(newQueue);
    }
    slot->queue->Reset(requestedQueue, VN_MONITOR_DROP_HUB_FANOUT, slot->epoch,
                       slot->generation, SymbolStreamKind(*slot));
    slot->queue->recordingReceiver = recordingReceiver;
    slot->queuePtr.store(slot->queue.get(), std::memory_order_release);
    slot->live.store(true, std::memory_order_release);
    if (eventCatalog) {
        owner.eventObservers.fetch_add(1, std::memory_order_release);
    }
    if (slot->executionFactVersion != 0 && (acceptedMask & VN_MONITOR_STREAM_EVENT) != 0)
        owner.executionObservers.fetch_add(1, std::memory_order_release);
    if (identityRequested) ++owner.identityObservers;
    if (symbolsRequested) ++owner.symbolObservers;
    if (owner.observerCount.fetch_add(1, std::memory_order_release) == 0) {
        owner.lifecycleSeen = {};
        owner.lifecycleSeenNext = 0;
    }
    *outHandle = PackHandle(slotIndex, slot->generation);
    lock.unlock();
    RefreshIdentityBoundary();
    // DSP three-quantity payload cannot fit frozen TimeSeries/Aggregate.
    // Publish one capability-unavailable DROP after the session is live.
    if (!eventCatalog && !recordingReceiver) {
        harken::ObserveMonitorDspGap();
    }
    return VN_OK;
}

VnResult OpenSession(const VnMonitorSessionDesc* desc,
                     VnMonitorSessionHandle* outHandle) {
    return OpenSessionWithPermissions(desc, false, 0, 0, outHandle);
}

VnResult OpenRemoteSession(const VnMonitorSessionDesc* desc,
                           std::uint32_t permissionMask,
                           std::uint64_t principalHash,
                           VnMonitorSessionHandle* outHandle) {
    return OpenSessionWithPermissions(desc, true, permissionMask,
                                      principalHash, outHandle);
}

VnResult OpenEventSession(const VnMonitorSessionDesc* desc,
                          VnMonitorSessionHandle* outHandle,
                          bool remote, std::uint64_t principalHash, std::uint32_t remoteGrant) {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)desc;
    (void)outHandle;
    (void)remote;
    (void)principalHash;
    (void)remoteGrant;
    return VN_MONITOR_UNSUPPORTED;
#else
    std::uint32_t version = 0;
    const auto versionResult = ReadExecutionVersion(desc, version);
    if (versionResult != VN_OK) return versionResult;
    if (desc->requestRawAudio != 0) return VN_INVALID_ARG;
    if (version == 0) {
        if (desc->requestedStreamKindMask != VN_MONITOR_STREAM_EVENT || desc->requestSymbols != 0)
            return VN_INVALID_ARG;
    } else if ((desc->requestedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0 ||
        (desc->requestedStreamKindMask & ~(VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE)) != 0) {
        return VN_INVALID_ARG;
    }
    return OpenSessionWithPermissions(desc, remote,
                                      version == 0 ? VN_MONITOR_PERM_OBSERVE_EVENTS : remoteGrant,
                                      principalHash, outHandle, true);
#endif
}

VnResult SealEventSession(VnMonitorSessionHandle handle) {
    const auto gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) return gate;
    auto& owner = Owner();
    if (owner.engine->callbackDispatchDepth != 0) return VN_BACKPRESSURE;
    RefreshIdentityBoundary();
    std::lock_guard<std::mutex> lock(owner.mutex);
    auto* session = FindLive(owner, handle);
    if (!session) return VN_MONITOR_STALE_SESSION;
    if (!session->eventCatalog) return VN_INVALID_ARG;
    if (session->eventStoppedNs != 0) return VN_OK;
    // Identity and symbol endpoints must include the outer Update transaction.
    // Pure EVENT sessions retain their synchronous seal and restart behavior.
    if (session->executionFactVersion != 0 && owner.identityMutationDepth != 0 &&
        ((session->acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0 || WantsSymbols(*session))) {
        if (!session->sealRequested) ++owner.pendingIdentitySeals;
        session->sealRequested = true;
    }
    else SealAtIdentityBoundary(owner, *session);
    return VN_OK;
}

VnResult GetEventSessionStatus(VnMonitorSessionHandle handle,
                               VnMonitorEventSessionStatus* outStatus) {
    if (outStatus == nullptr) return VN_INVALID_ARG;
    const auto requestedSize = outStatus->structSize;
    if (requestedSize != sizeof(VnMonitorEventSessionStatus) &&
        requestedSize != sizeof(VnMonitorEventSessionStatusV2)) return VN_INVALID_ARG;
    const auto gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) return gate;
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    auto* session = FindLive(owner, handle);
    if (!session) return VN_MONITOR_STALE_SESSION;
    if (!session->eventCatalog) return VN_INVALID_ARG;
    VnMonitorEventSessionStatusV2 status{};
    status.structSize = requestedSize;
    if (session->sealRequested) status.state = VN_MONITOR_EVENT_SESSION_SEALED;
    status.monitorSessionEpoch = session->epoch;
    status.startedTimestampNs = session->eventStartedNs;
    status.stoppedTimestampNs = session->eventStoppedNs;
    const auto logDrops = owner.engine->logRing.DroppedCount() +
        owner.engine->logicLogRing.DroppedCount() + owner.engine->logRing.QuantumBudgetDropCount();
    status.sourceDroppedRecords = session->eventSourceDrops + logDrops - session->logDropBaseline +
        session->identityDrops.recordCount + session->baselineDrops.recordCount + session->symbolDrops.recordCount;
    std::lock_guard<std::mutex> queueLock(session->queue->mutex);
    status.deliveryDroppedRecords = session->queue->droppedRecords;
    if (session->eventStoppedNs != 0) {
        bool executionDrained = true;
        if (session->executionFactVersion != 0)
            for (unsigned i = 0; i != owner.executionProducers.size(); ++i)
                executionDrained &= owner.executionProducers[i].Completed() >= session->executionEnds[i];
        const bool identityDrained = session->executionFactVersion == 0 ||
            (session->acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) == 0 ||
            (!session->identity.requested && !session->identity.active &&
                owner.identityProducer.Completed() >= session->identityEnd);
        const bool symbolDrained = !WantsSymbols(*session) ||
            (owner.symbolProducer.Completed() >= session->symbolEnd && session->queue->deferredSymbols.empty());
        const bool legacyDrained = executionDrained && identityDrained && symbolDrained && owner.engine->logRing.ConsumedCount() >= session->sealedAudioLogs &&
            owner.engine->logicLogRing.ConsumedCount() >= session->sealedLogicLogs &&
            owner.lastEventFanoutSequence.load(std::memory_order_acquire) >= session->lastEventSequence;
        const bool drained = (session->executionFactVersion != 0 ? RecordingSourcesDrained(owner, *session, false) : legacyDrained) &&
            session->queue->deferredSymbols.empty() && session->queue->records.empty();
        if (drained && !session->eventDrained) {
            session->eventDrained = true;
            session->finalSourceDrops = status.sourceDroppedRecords;
        }
        status.state = session->eventDrained ? VN_MONITOR_EVENT_SESSION_DRAINED : VN_MONITOR_EVENT_SESSION_SEALED;
        if (session->eventDrained) status.sourceDroppedRecords = session->finalSourceDrops;
    }
    status.acceptedExecutionFactVersion = session->executionFactVersion;
    std::copy(session->supportedFactKinds.begin(), session->supportedFactKinds.end(), status.supportedFactKinds);
    status.baselineAvailability = session->baselineAvailability;
    status.symbolAvailability = session->symbolAvailability;
    status.requestedPermissionMask = session->requestedPermissionMask;
    status.acceptedPermissionMask = session->acceptedPermissionMask;
    status.deniedPermissionMask = session->deniedPermissionMask;
    status.unsupportedPermissionMask = session->unsupportedPermissionMask;
    status.acceptedStreamKindMask = session->acceptedStreamKindMask;
    status.symbolsDecision = session->symbolsDecision;
    status.baselineStateCutId = session->executionFactVersion != 0 ? session->cut.id : 0;
    const auto requestUnavailable = owner.engine->monitorRequests.UnavailableCount();
    const auto hostUnavailable = owner.engine->monitorEmitters.UnavailableCount();
    status.identityUnavailableCount = UINT64_MAX - requestUnavailable < hostUnavailable
        ? UINT64_MAX : requestUnavailable + hostUnavailable;
    for (const auto& producer : owner.executionProducers) {
        const auto unavailable = (session->acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) != 0 ? producer.Unavailable() : 0;
        status.identityUnavailableCount = UINT64_MAX - status.identityUnavailableCount < unavailable
            ? UINT64_MAX : status.identityUnavailableCount + unavailable;
    }
    if ((session->acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0)
        for (auto unavailable : {owner.identityProducer.Unavailable(), owner.identityMirror.Unavailable()})
            status.identityUnavailableCount = UINT64_MAX - status.identityUnavailableCount < unavailable
                ? UINT64_MAX : status.identityUnavailableCount + unavailable;
    const auto symbolUnavailable = SymbolUnavailable(owner, *session);
    status.identityUnavailableCount = UINT64_MAX - status.identityUnavailableCount < symbolUnavailable
        ? UINT64_MAX : status.identityUnavailableCount + symbolUnavailable;
    std::memcpy(outStatus, &status, requestedSize);
    return VN_OK;
}

bool EventCatalogEnabled() {
    return Owner().eventObservers.load(std::memory_order_acquire) != 0 && LogicThreadReady();
}

bool ExecutionObservationEnabled() noexcept {
    return Owner().executionObservers.load(std::memory_order_acquire) != 0;
}

std::uint64_t ExecutionEngineInstance() noexcept {
    return LogicThreadReady() ? Owner().engine->monitorRequests.EngineInstance() : 0;
}

bool PublishExecutionFact(MonitorExecutionFact fact, bool audioProducer) noexcept {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)fact;
    (void)audioProducer;
    return false;
#else
    auto& owner = Owner();
    if (!ExecutionObservationEnabled() || (!audioProducer && !LogicThreadReady())) return false;
    // Identity cuts have per-session staging/watermarks. Keep this source
    // sequence EVENT-only so a dropped STATE item cannot taint EVENT grants.
    if (fact.kind == VN_MONITOR_FACT_IDENTITY_UPSERT || fact.kind == VN_MONITOR_FACT_IDENTITY_TOMBSTONE) return false;
    auto& ring = audioProducer ? owner.producerRing : owner.logicRing;
    const bool result = owner.executionProducers[audioProducer ? 1 : 0].Publish(ring, fact,
        owner.engine->monitorRequests.EngineInstance());
    owner.WakeDrain();
    return result;
#endif
}

bool PublishEventDetail(const VnMonitorEventDetail& detail,
                         std::uint32_t entityKind, VnObjectId stableId,
                         VnObjectId projectId,
                         std::uint64_t worldGeneration,
                         std::uint64_t instanceHandle, std::uint64_t instanceGeneration,
                         std::uint64_t programFingerprint,
                         std::uint32_t logSource, std::uint64_t logTicket) {
    if (!EventCatalogEnabled()) return false;
    auto& owner = Owner();
    LifecycleEnvelope envelope{};
    envelope.sourceSequence = NextLogicSourceSequence();
    envelope.worldGeneration = worldGeneration;
    envelope.executeFrame = CurrentExecuteFrame();
    envelope.entityKind = entityKind;
    envelope.stableId = stableId;
    envelope.instanceHandle = instanceHandle;
    envelope.instanceGeneration = instanceGeneration;
    envelope.programSemanticFingerprint = programFingerprint;
    if (detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE) {
        envelope.reason = static_cast<std::uint32_t>(detail.result);
        envelope.playableInstanceHandle = detail.arg0;
        envelope.actionInstanceHandle = detail.arg1;
    }
    constexpr auto payloadBytes = sizeof(VnMonitorEventDetailV2);
    constexpr auto bytes = PackedRecordBytes(payloadBytes);
    std::uint8_t packed[bytes]{};
    VnMonitorRecordHeader header{};
    FillLifecycleHeader(&header, envelope, bytes, payloadBytes,
                        VN_MONITOR_RECORD_EVENT_DETAIL, VN_MONITOR_STREAM_EVENT);
    header.recordVersion = 2;
    header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
    PackHeader(packed, header);
    auto* payload = packed + kPackedEnvelopeBytes;
    WriteU32(payload, payloadBytes);
    WriteU32(payload + 4, detail.category);
    WriteU32(payload + 8, detail.code);
    WriteU32(payload + 12, detail.level);
    WriteU32(payload + 16, static_cast<std::uint32_t>(detail.result));
    WriteU64(payload + 24, detail.timestampNs);
    WriteU64(payload + 32, detail.arg0);
    WriteU64(payload + 40, detail.arg1);
    WriteU32(payload + 48, detail.valueBits);
    WriteU32(payload + 52, detail.textBytes);
    std::memcpy(payload + 56, detail.text, sizeof(detail.text));
    const auto projectBytes = ObjectIdBytes(projectId);
    std::memcpy(payload + sizeof(detail), projectBytes.data(), projectBytes.size());
    std::lock_guard<std::mutex> lock(owner.mutex);
    const MonitorProducerOrigin origin{logSource, 0, logTicket};
    const bool published = PublishToRing(owner.logicRing, packed, bytes, origin);
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || !session.eventCatalog ||
            session.eventDrained ||
            (!(session.executionFactVersion != 0 && logSource >= 3 && logSource <= 4) &&
             !session.recordingReceiver && (detail.timestampNs < session.eventStartedNs ||
                (session.eventStoppedNs != 0 && detail.timestampNs > session.eventStoppedNs)))) continue;
        if (session.executionFactVersion != 0) {
            if (logSource >= 3 && logSource <= 4) {
                const auto i = logSource - 3;
                const auto end = i == 0 ? session.sealedAudioLogs : session.sealedLogicLogs;
                if (logTicket <= session.logStarts[i] || (session.eventStoppedNs != 0 && logTicket > end)) continue;
                if (published) session.logDownstreamEnds[i] = owner.logicRing.Published();
            } else if (session.recordingReceiver && session.eventStoppedNs != 0) continue;
        }
        if (session.executionFactVersion != 0 && ExecutionReplacesLegacyEvent(detail.category, detail.code)) continue;
        if (published) session.lastEventSequence = envelope.sourceSequence;
        else ++session.eventSourceDrops;
    }
    return published;
}

void ObserveOperation(std::uint32_t operation, VnResult result,
                       std::uint32_t entityKind, VnObjectId stableId,
                       std::uint64_t handle, std::uint64_t arg0, std::uint64_t arg1,
                       float value, const char* text) {
    if (!EventCatalogEnabled()) return;
    VnMonitorEventDetail detail{};
    detail.structSize = sizeof(detail);
    detail.category = VN_MONITOR_EVENT_CATEGORY_OPERATION;
    detail.code = operation;
    detail.level = result == VN_OK ? VN_LOG_INFO : VN_LOG_ERROR;
    detail.result = result;
    detail.timestampNs = LogTimestampNs();
    detail.arg0 = arg0;
    detail.arg1 = arg1;
    std::memcpy(&detail.valueBits, &value, sizeof(value));
    if (text) {
        while (detail.textBytes < sizeof(detail.text) && text[detail.textBytes] != '\0') {
            detail.text[detail.textBytes] = text[detail.textBytes];
            ++detail.textBytes;
        }
    }
    auto& engine = *Owner().engine;
    const bool packed = engine.packComposition.Authority() == PackAuthorityMode::Pack;
    auto world = packed ? engine.packComposition.CompositeGeneration() : engine.variables.CurrentWorldGeneration();
    VnObjectId projectId = engine.CurrentProjectId();
    std::uint64_t programFingerprint = 0;
    std::uint64_t generation = 0;
    if (entityKind == VN_MONITOR_ENTITY_ACTION) {
        ActionFiringSnapshot firing;
        if (handle && engine.actionFirings.Capture(handle, &firing)) {
            stableId = firing.actionId;
            world = firing.worldGeneration;
            projectId = firing.owner.projectId;
            programFingerprint = firing.programSemanticFingerprint;
            generation = handle >> 32;
        } else if (stableId != VnObjectId{} && operation != VN_MONITOR_OPERATION_STOP_ACTION) {
            if (packed) {
                if (const auto* action = engine.packComposition.FindAction(stableId))
                    programFingerprint = action->programSemanticFingerprint;
            } else {
                for (std::uint32_t i = 0; i < engine.variables.ActionProgramCount(); ++i) {
                    const auto& action = engine.variables.ActionProgramAt(i);
                    if (action.actionStableId == stableId) {
                        programFingerprint = action.programSemanticFingerprint;
                        break;
                    }
                }
            }
        }
    } else if (entityKind == VN_MONITOR_ENTITY_PLAYABLE) {
        PlayableRootMetadata metadata;
        if (handle && engine.playableInstances.CaptureRootMetadata(handle, &metadata)) {
            projectId = metadata.projectId;
            stableId = metadata.definitionStableId;
            world = metadata.worldGeneration;
            programFingerprint = metadata.programSemanticFingerprint;
            generation = handle >> 32;
        }
    }
    PublishEventDetail(detail, entityKind, stableId, projectId, world, handle, generation,
                        programFingerprint);
}

void ObserveDiagnostic(std::uint32_t level, std::uint32_t code,
                        std::uint64_t arg0, std::uint64_t arg1,
                        std::uint64_t timestampNs, VnObjectId objectId, VnObjectId projectId,
                        const char* text, std::uint32_t logSource, std::uint64_t logTicket) {
    if (!EventCatalogEnabled()) return;
    const bool eligible = level >= VN_LOG_WARN;
    VnMonitorEventDetail detail{};
    detail.structSize = sizeof(detail);
    detail.category = VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC;
    detail.code = code;
    detail.level = level;
    detail.timestampNs = timestampNs;
    detail.arg0 = arg0;
    detail.arg1 = arg1;
    if (text) {
        while (detail.textBytes < sizeof(detail.text) && text[detail.textBytes] != '\0') {
            detail.text[detail.textBytes] = text[detail.textBytes];
            ++detail.textBytes;
        }
    }
    if (eligible) PublishEventDetail(detail, VN_MONITOR_ENTITY_NONE, objectId, projectId, 0, 0, 0, 0, logSource, logTicket);
    if (logSource < 3 || logSource > 4 || logTicket == 0) return;
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    const auto index = logSource - 3;
    const auto previous = owner.logTranscriptLast[index];
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || !session.recordingReceiver || session.eventDrained ||
            (session.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0) continue;
        const auto first = std::max(previous, session.logStarts[index]);
        const auto end = session.eventStoppedNs == 0 ? logTicket : std::min(logTicket,
            index == 0 ? session.sealedAudioLogs : session.sealedLogicLogs);
        if (end > first) session.logLost[index] += end - first - (end == logTicket ? 1u : 0u);
        if (!eligible && logTicket > session.logStarts[index] && end == logTicket) ++session.logFiltered[index];
    }
    owner.logTranscriptLast[index] = logTicket;
    owner.logProjectionCompleted[index].store(logTicket, std::memory_order_release);
    owner.WakeDrain();
}

void CompleteLogTranscription(std::uint32_t source, std::uint64_t consumed) {
    if (source < 3 || source > 4 || !EventCatalogEnabled()) return;
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    const auto index = source - 3;
    const auto previous = owner.logTranscriptLast[index];
    for (auto& session : owner.sessions) {
        if (!session.live.load(std::memory_order_acquire) || !session.recordingReceiver || session.eventDrained ||
            (session.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0) continue;
        const auto end = session.eventStoppedNs == 0 ? consumed : std::min(consumed,
            index == 0 ? session.sealedAudioLogs : session.sealedLogicLogs);
        const auto first = std::max(previous, session.logStarts[index]);
        if (end > first) session.logLost[index] += end - first;
    }
    owner.logTranscriptLast[index] = std::max(previous, consumed);
    owner.logProjectionCompleted[index].store(owner.logTranscriptLast[index], std::memory_order_release);
    owner.WakeDrain();
}

VnResult CloseSession(VnMonitorSessionHandle handle) {
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    RefreshIdentityBoundary();
    std::lock_guard<std::mutex> lock(owner.mutex);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    DisconnectHolderLocked(owner, handle);
    RetireSessionLocked(owner, *session);
    return VN_OK;
}

VnResult GetSubscriptionStatus(VnMonitorSessionHandle handle,
                               VnMonitorSubscriptionStatus* outStatus) {
    if (outStatus == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    const auto requestedSize = outStatus->structSize;
    if (requestedSize != sizeof(VnMonitorSubscriptionStatus) &&
        requestedSize != sizeof(VnMonitorSubscriptionStatusV2)) {
        return VN_INVALID_ARG;
    }
    auto* destination = outStatus;
    VnMonitorSubscriptionStatus legacy{};
    std::memcpy(&legacy, outStatus, sizeof(legacy));
    outStatus = &legacy;
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    const std::uint32_t requestedMask = session->requestedStreamKindMask;
    const std::uint32_t requestedPerm = session->requestedPermissionMask;
    const std::uint32_t resolvedMask = session->acceptedStreamKindMask;
    const std::uint32_t acceptedPerm = session->acceptedPermissionMask;
    const std::uint32_t unsupportedPerm = session->unsupportedPermissionMask;
    const std::uint32_t deniedPerm = session->deniedPermissionMask;
    const std::uint32_t callerCapacity = outStatus->baselineCapacity;
    VnMonitorStreamBaseline* callerBaselines = outStatus->baselines;
    const std::uint32_t matchingCount =
         ((resolvedMask & VN_MONITOR_STREAM_TIME_SERIES) ? 1u : 0u) +
         ((resolvedMask & VN_MONITOR_STREAM_STATE) ? (session->executionFactVersion != 0 ? 2u : 1u) : 0u) +
         (WantsSymbols(*session) ? 1u : 0u);
    if (matchingCount > 0 &&
        ((callerBaselines == nullptr && callerCapacity != 0) ||
          (callerBaselines != nullptr && callerCapacity < matchingCount))) {
         outStatus->baselineCount = matchingCount;
         std::memcpy(reinterpret_cast<std::uint8_t*>(destination) +
             offsetof(VnMonitorSubscriptionStatus, baselineCount), &matchingCount, sizeof(matchingCount));
         return VN_MONITOR_BUFFER_TOO_SMALL;
    }
    std::memset(outStatus, 0, sizeof(*outStatus));
    outStatus->structSize = sizeof(*outStatus);
    outStatus->requestedStreamKindMask = requestedMask;
    outStatus->resolvedStreamKindMask = resolvedMask;
    outStatus->subscriptionGeneration = 1;
    outStatus->requestedPermissionMask = requestedPerm;
    outStatus->acceptedPermissionMask = acceptedPerm;
    outStatus->deniedPermissionMask = deniedPerm;
    outStatus->unsupportedPermissionMask = unsupportedPerm;
    outStatus->monitorSessionEpoch = session->epoch;
    outStatus->rawAudioDecision = session->rawAudioDecision;
    outStatus->symbolsDecision = session->symbolsDecision;
    outStatus->baselineCapacity = callerCapacity;
    outStatus->baselines = callerBaselines;
    outStatus->baselineCount = matchingCount;
    if (matchingCount > 0 && callerBaselines != nullptr &&
        callerCapacity >= matchingCount) {
        std::uint32_t written = 0;
        if (resolvedMask & VN_MONITOR_STREAM_TIME_SERIES) {
            std::memset(&callerBaselines[written], 0, sizeof(callerBaselines[written]));
            callerBaselines[written].structSize = sizeof(callerBaselines[written]);
            callerBaselines[written].streamId = kSyntheticStreamId;
            callerBaselines[written].firstExpectedSourceSequence =
                session->firstExpectedTimeSeriesSequence;
            callerBaselines[written].streamKind = VN_MONITOR_STREAM_TIME_SERIES;
            ++written;
        }
        if (resolvedMask & VN_MONITOR_STREAM_STATE) {
            std::memset(&callerBaselines[written], 0, sizeof(callerBaselines[written]));
            callerBaselines[written].structSize = sizeof(callerBaselines[written]);
            callerBaselines[written].streamId = session->executionFactVersion != 0 ? kIdentityDeltaStream : kSyntheticStateStreamId;
            callerBaselines[written].firstExpectedSourceSequence = session->executionFactVersion != 0
                ? (session->identityStart == UINT64_MAX ? 0 : session->identityStart + 1) : session->firstExpectedStateSequence;
            callerBaselines[written].streamKind = VN_MONITOR_STREAM_STATE;
            ++written;
            if (session->executionFactVersion != 0) {
                callerBaselines[written] = {};
                callerBaselines[written].structSize = sizeof(callerBaselines[written]);
                callerBaselines[written].streamId = kIdentityBaselineStream;
                callerBaselines[written].firstExpectedSourceSequence = session->cut.beginSourceSequence;
                callerBaselines[written].streamKind = VN_MONITOR_STREAM_STATE;
                ++written;
            }
        }
        if (WantsSymbols(*session)) {
            callerBaselines[written] = {};
            callerBaselines[written].structSize = sizeof(callerBaselines[written]);
            callerBaselines[written].streamId = kHostSymbolStream;
            callerBaselines[written].streamKind = SymbolStreamKind(*session);
            callerBaselines[written].firstExpectedSourceSequence =
                session->symbolStart == UINT64_MAX ? 0 : session->symbolStart + 1;
        }
    }
    VnMonitorSubscriptionStatusV2 status{};
    std::memcpy(&status, &legacy, sizeof(legacy));
    status.structSize = requestedSize;
    status.acceptedExecutionFactVersion = session->executionFactVersion;
    status.baselineAvailability = session->baselineAvailability;
    status.symbolAvailability = session->symbolAvailability;
    std::copy(session->supportedFactKinds.begin(), session->supportedFactKinds.end(), status.supportedFactKinds);
    status.baselineStateCutId = session->executionFactVersion != 0 ? session->cut.id : 0;
    const auto requestUnavailable = owner.engine->monitorRequests.UnavailableCount();
    const auto hostUnavailable = owner.engine->monitorEmitters.UnavailableCount();
    status.identityUnavailableCount = UINT64_MAX - requestUnavailable < hostUnavailable
        ? UINT64_MAX : requestUnavailable + hostUnavailable;
    for (const auto& producer : owner.executionProducers) {
        const auto unavailable = (session->acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) != 0 ? producer.Unavailable() : 0;
        status.identityUnavailableCount = UINT64_MAX - status.identityUnavailableCount < unavailable
            ? UINT64_MAX : status.identityUnavailableCount + unavailable;
    }
    if ((session->acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) != 0)
        for (auto unavailable : {owner.identityProducer.Unavailable(), owner.identityMirror.Unavailable()})
            status.identityUnavailableCount = UINT64_MAX - status.identityUnavailableCount < unavailable
                ? UINT64_MAX : status.identityUnavailableCount + unavailable;
    const auto symbolUnavailable = SymbolUnavailable(owner, *session);
    status.identityUnavailableCount = UINT64_MAX - status.identityUnavailableCount < symbolUnavailable
        ? UINT64_MAX : status.identityUnavailableCount + symbolUnavailable;
    std::memcpy(destination, &status, requestedSize);
    return VN_OK;
}

VnResult Poll(VnMonitorSessionHandle handle, const VnMonitorPollDesc* desc,
              VnMonitorPollResult* outResult) {
    if (desc == nullptr || outResult == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(true);
    if (gate != VN_OK) {
        return gate;
    }
    if (!ValidStructSize(desc->structSize, sizeof(VnMonitorPollDesc)) ||
        !ValidStructSize(outResult->structSize, sizeof(VnMonitorPollResult))) {
        return VN_INVALID_ARG;
    }
    if (!ReservedZero(desc->reserved0)) {
        return VN_INVALID_ARG;
    }
    if (desc->bufferBytes > 0 && desc->buffer == nullptr) {
        return VN_INVALID_ARG;
    }

    ProcessOwner& owner = Owner();
    Session* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        session = FindLive(owner, handle);
        if (session == nullptr) {
            return VN_MONITOR_STALE_SESSION;
        }
        if (session->resyncRequired.load(std::memory_order_acquire)) {
            return VN_MONITOR_RESYNC_REQUIRED;
        }
        if (session->pollInFlight.fetch_add(1, std::memory_order_acq_rel) !=
            0) {
            session->pollInFlight.fetch_sub(1, std::memory_order_acq_rel);
            return VN_MONITOR_CONCURRENT_POLL;
        }
    }

    owner.pollEntered.fetch_add(1, std::memory_order_acq_rel);
    while (owner.pollStall.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::memset(outResult, 0, sizeof(*outResult));
    outResult->structSize = sizeof(*outResult);

    ConsumerQueue* queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        const std::uint64_t generation = HandleGeneration(handle);
        if (session != nullptr &&
            session->live.load(std::memory_order_acquire) &&
            session->generation == generation) {
            queue = session->queuePtr.load(std::memory_order_acquire);
        } else {
            for (auto& item : owner.retired) {
                if (item->generation == generation) {
                    queue = item->queue.get();
                    break;
                }
            }
        }
    }

    VnResult pollStatus = VN_OK;
    if (queue != nullptr) {
        std::vector<PackedRecord> copied;
        std::uint64_t remaining = 0;
        std::uint64_t nextBytes = 0;
        {
            std::lock_guard<std::mutex> queueLock(queue->mutex);
            remaining = queue->records.size();
            if (remaining > 0) {
                nextBytes = queue->records.front().bytes.size();
            }
            std::uint64_t written = 0;
            auto cursor = queue->records.begin();
            while (cursor != queue->records.end()) {
                const std::uint64_t recordBytes = cursor->bytes.size();
                if (desc->buffer == nullptr ||
                    written + recordBytes > desc->bufferBytes) {
                    nextBytes = recordBytes;
                    break;
                }
                copied.push_back(*cursor);
                written += recordBytes;
                ++cursor;
            }
            if (!copied.empty()) {
                queue->records.erase(queue->records.begin(),
                                     queue->records.begin() +
                                         static_cast<std::ptrdiff_t>(copied.size()));
                remaining = queue->records.size();
                queue->usedBytes -= written;
            }
        }
        if (copied.empty() && nextBytes > 0) {
            pollStatus = VN_MONITOR_BUFFER_TOO_SMALL;
            outResult->nextRecordBytes = nextBytes;
            outResult->recordsRemaining = remaining;
        } else if (!copied.empty()) {
            std::uint8_t* out = static_cast<std::uint8_t*>(desc->buffer);
            std::uint64_t written = 0;
            outResult->firstDeliverySequence =
                ReadPackedU64(copied.front().bytes.data(),
                              kPackedDeliverySequenceOffset);
            for (const PackedRecord& record : copied) {
                std::memcpy(out + written, record.bytes.data(),
                            record.bytes.size());
                written += record.bytes.size();
                outResult->lastDeliverySequence =
                    ReadPackedU64(record.bytes.data(),
                                  kPackedDeliverySequenceOffset);
            }
            outResult->recordsWritten =
                static_cast<std::uint32_t>(copied.size());
            outResult->bytesWritten = written;
            outResult->recordsRemaining = remaining;
            if (remaining > 0) {
                outResult->nextRecordBytes = nextBytes;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        const std::uint64_t generation = HandleGeneration(handle);
        if (session != nullptr &&
            session->live.load(std::memory_order_acquire) &&
            session->generation == generation) {
            session->pollInFlight.fetch_sub(1, std::memory_order_acq_rel);
        } else {
            for (auto& item : owner.retired) {
                if (item->generation == generation) {
                    item->pollInFlight.fetch_sub(1, std::memory_order_acq_rel);
                    break;
                }
            }
            owner.SweepRetiredLocked();
        }
    }
    return pollStatus;
}

VnResult GetConsumerStatus(VnMonitorSessionHandle handle,
                           VnMonitorDropSummary* outSummaries, uint32_t capacity,
                           uint32_t* requiredCount) {
    if (requiredCount == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    *requiredCount = 0;
    ConsumerQueue* queue = session->queuePtr.load(std::memory_order_acquire);
    if (queue == nullptr) {
        return VN_OK;
    }
    constexpr auto executionSourceOffset = 1 + ConsumerQueue::kDropStreamCount;
    constexpr auto identitySourceOffset = executionSourceOffset + 2;
    std::array<DropAccum, identitySourceOffset + 3> snapshots{};
    if (session->executionFactVersion == 0 || (session->acceptedStreamKindMask & ~VN_MONITOR_STREAM_STATE) != 0)
        snapshots[0] = owner.producerDrop.Snapshot();
    snapshots[identitySourceOffset] = session->identityDrops;
    snapshots[identitySourceOffset + 1] = session->baselineDrops;
    snapshots[identitySourceOffset + 2] = session->symbolDrops;
    {
        std::lock_guard<std::mutex> queueLock(queue->mutex);
        std::copy(queue->drops.begin(), queue->drops.end(), snapshots.begin() + 1);
    }
    // Source counters remain queryable even when their in-stream summary was
    // dropped. This also covers ordinary V2 sessions without Catalog status.
    std::copy(session->executionSourceDrops.begin(), session->executionSourceDrops.end(), snapshots.begin() + executionSourceOffset);
    for (const auto& snapshot : snapshots) if (snapshot.recordCount != 0) ++*requiredCount;
    if (*requiredCount == 0) return VN_OK;
    if (outSummaries == nullptr || capacity < *requiredCount) return VN_MONITOR_BUFFER_TOO_SMALL;
    std::uint32_t written = 0;
    for (const auto& snapshot : snapshots) {
        if (snapshot.recordCount == 0) continue;
        auto& summary = outSummaries[written++];
        summary = {};
        summary.structSize = sizeof(summary);
        summary.stage = snapshot.stage;
        summary.streamId = snapshot.streamId;
        summary.firstSourceSequence = snapshot.firstSourceSequence;
        summary.lastSourceSequence = snapshot.lastSourceSequence;
        summary.recordCount = snapshot.recordCount;
        summary.byteCount = snapshot.byteCount;
        summary.reason = snapshot.reason;
    }
    return VN_OK;
}

VnResult RequestStateCut(VnMonitorSessionHandle handle,
                         const VnMonitorStateCutDesc* desc,
                         uint64_t* outStateCutId) {
    if (desc == nullptr || outStateCutId == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    if (!ValidStructSize(desc->structSize, sizeof(VnMonitorStateCutDesc))) {
        return VN_INVALID_ARG;
    }
    const HostPrincipal principal = CurrentHostPrincipal();
    if (!principal.authenticated ||
        (principal.observeMask & VN_MONITOR_STREAM_STATE) == 0) {
        return VN_MONITOR_PERMISSION_DENIED;
    }
    ProcessOwner& owner = Owner();
    // Callback reentrancy and an open outer transaction are not valid cuts.
    if (!IdentitySafeBoundary(owner)) return VN_BACKPRESSURE;
    RefreshIdentityBoundary();
    std::uint64_t expectedEpoch = 0;
    std::uint64_t expectedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        Session* session = FindLive(owner, handle);
        if (session == nullptr) {
            return VN_MONITOR_STALE_SESSION;
        }
        if ((session->acceptedStreamKindMask & VN_MONITOR_STREAM_STATE) == 0) {
            session->cut = {};
            session->cut.result = VN_MONITOR_STATE_CUT_ABORTED;
            return VN_MONITOR_PERMISSION_DENIED;
        }
        if (session->eventStoppedNs != 0) return VN_INVALID_ARG;
        if (session->executionFactVersion != 0) {
            const auto result = FreezeIdentityCut(owner, *session, desc->requestedCapacityHint);
            if (result == VN_OK) *outStateCutId = session->cut.id;
            return result;
        }
        if (desc->requestedCapacityHint != 0 &&
            desc->requestedCapacityHint < kStateCutRequiredRecords) {
            session->cut = {};
            session->cut.id = owner.nextDropSourceSequence;
            session->cut.result = VN_MONITOR_STATE_CUT_ABORTED;
            session->cut.settled = true;
            *outStateCutId = session->cut.id;
            return VN_OK;
        }
        if (session->queuePtr.load(std::memory_order_acquire) == nullptr) {
            return VN_MONITOR_STALE_SESSION;
        }
        expectedEpoch = session->epoch;
        expectedGeneration = session->generation;
        session->cut = {};
        session->cut.id = owner.nextStateSequence;
        session->cut.beginSourceSequence = owner.nextStateSequence;
    }

    std::uint8_t packed[kScratchBytes];
    StateCut cut{};
    cut.id = owner.nextStateSequence;
    cut.beginSourceSequence = cut.id;
    const std::uint32_t beginSize = PackStateCutBegin(
        packed, sizeof(packed), cut.beginSourceSequence, cut.id, expectedEpoch);
    owner.nextStateSequence += 1;
    cut.beginEnqueued = beginSize > 0 && PublishLogicPacked(packed, beginSize);

    const std::uint64_t checksum = EmptyStateCutChecksum(cut.id);
    cut.endSourceSequence = owner.nextStateSequence;
    const std::uint32_t endSize = PackStateCutEnd(
        packed, sizeof(packed), cut.endSourceSequence, cut.id, expectedEpoch,
        VN_MONITOR_STATE_CUT_COMPLETE, checksum);
    owner.nextStateSequence += 1;
    if (owner.dropStateCutEnd) {
        owner.dropStateCutEnd = false;
        cut.endEnqueued = false;
    } else {
        cut.endEnqueued = endSize > 0 && PublishLogicPacked(packed, endSize);
    }
    cut.checksum = checksum;
    cut.result = VN_MONITOR_STATE_CUT_ABORTED;
    cut.emittedCount = 0;
    if (!cut.beginEnqueued && !cut.endEnqueued) {
        cut.checksum = 0;
        cut.settled = true;
    }

    std::lock_guard<std::mutex> lock(owner.mutex);
    Session* live = FindLive(owner, handle);
    if (live == nullptr || live->generation != expectedGeneration ||
        live->epoch != expectedEpoch) {
        return VN_MONITOR_STALE_SESSION;
    }
    const bool beginDelivered = live->cut.beginDelivered;
    const bool endDelivered = live->cut.endDelivered;
    live->cut = cut;
    live->cut.beginDelivered = beginDelivered;
    live->cut.endDelivered = endDelivered;
    *outStateCutId = cut.id;
    return VN_OK;
}

VnResult GetStateCutStatus(VnMonitorSessionHandle handle, uint64_t stateCutId,
                           VnMonitorStateCutStatus* outStatus) {
    if (outStatus == nullptr) {
        return VN_INVALID_ARG;
    }
    const VnResult gate = RequireEngineAndLogicThread(false);
    if (gate != VN_OK) {
        return gate;
    }
    if (!ValidStructSize(outStatus->structSize,
                         sizeof(VnMonitorStateCutStatus))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (session->cut.id == 0 || session->cut.id != stateCutId) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (session->executionFactVersion == 0) SettleStateCutFromDelivery(
        &session->cut, owner.lastLogicFanoutSequence.load(std::memory_order_acquire));
    if (!session->cut.settled) {
        return VN_MONITOR_STALE_SESSION;
    }
    std::memset(outStatus, 0, sizeof(*outStatus));
    outStatus->structSize = sizeof(*outStatus);
    outStatus->result = session->cut.result;
    outStatus->stateCutId = session->cut.id;
    outStatus->worldGeneration = 0;
    outStatus->capturePlanGeneration = 0;
    outStatus->emittedCount = session->cut.emittedCount;
    outStatus->checksum = session->cut.checksum;
    return VN_OK;
}

VnResult AcquireCaptureControl(VnMonitorSessionHandle handle,
                               VnMonitorCaptureControlStatus* outStatus) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (outStatus == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outStatus->structSize,
                         sizeof(VnMonitorCaptureControlStatus))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    RefreshIdentityBoundary();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if ((session->acceptedPermissionMask & kCaptureControlPermission) == 0) {
        return VN_MONITOR_PERMISSION_DENIED;
    }
    if (owner.lease.held && owner.lease.holder != handle) {
        return VN_IN_USE;
    }
    if (owner.lease.held) StopOwnedRecordingLocked(owner, handle, owner.lease.generation, kLeaseReasonAcquire);
    owner.lease.generation += 1;
    owner.lease.held = true;
    owner.lease.holder = handle;
    owner.lease.principalHash = PrincipalHashFor(*session);
    owner.lease.permissions = kCaptureControlPermission;
    owner.lease.expiryExecuteFrame = 0;
    owner.leaseExpiryFrame.store(0, std::memory_order_release);
    owner.lease.lastChangeReason = kLeaseReasonAcquire;
    FillLeaseStatus(owner, outStatus);
    return VN_OK;
}

VnResult ReleaseCaptureControl(VnMonitorSessionHandle handle,
                               uint64_t leaseGeneration) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    RefreshIdentityBoundary();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) {
        return VN_MONITOR_STALE_LEASE;
    }
    StopOwnedRecordingLocked(owner, handle, leaseGeneration, kLeaseReasonRelease);
    owner.lease.held = false;
    owner.lease.holder = 0;
    owner.lease.principalHash = 0;
    owner.lease.permissions = 0;
    owner.lease.lastChangeReason = kLeaseReasonRelease;
    return VN_OK;
}

VnResult GetCaptureControlStatus(VnMonitorSessionHandle handle,
                                 VnMonitorCaptureControlStatus* outStatus) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (outStatus == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outStatus->structSize,
                         sizeof(VnMonitorCaptureControlStatus))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    if (FindLive(owner, handle) == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    FillLeaseStatus(owner, outStatus);
    return VN_OK;
}

VnResult ValidateCapturePlan(VnMonitorSessionHandle handle,
                             uint64_t leaseGeneration,
                             const VnMonitorCapturePlan* plan,
                             VnMonitorEffectiveCapturePlan* outAccepted) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (plan == nullptr || outAccepted == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outAccepted->structSize,
                         sizeof(VnMonitorEffectiveCapturePlan))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) {
        return VN_MONITOR_STALE_LEASE;
    }
    VnMonitorCapturePlan accepted{};
    const VnResult check = CopyValidatedPlan(plan, &accepted, nullptr);
    if (check != VN_OK) {
        return check;
    }
    WriteEffective(owner, outAccepted);
    outAccepted->accepted = accepted;
    return VN_OK;
}

VnResult ApplyCapturePlan(VnMonitorSessionHandle handle,
                          uint64_t leaseGeneration,
                          const VnMonitorCapturePlan* plan,
                          VnMonitorEffectiveCapturePlan* outAccepted) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (plan == nullptr || outAccepted == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outAccepted->structSize,
                         sizeof(VnMonitorEffectiveCapturePlan))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if ((session->acceptedPermissionMask & kCaptureControlPermission) == 0) {
        return VN_MONITOR_PERMISSION_DENIED;
    }
    if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) {
        return VN_MONITOR_STALE_LEASE;
    }
    if (owner.pendingSlot >= 0) {
        return VN_IN_USE;
    }
    const std::int32_t slotIndex = owner.FindFreeNormalSlotLocked();
    if (slotIndex < 0) {
        return VN_MONITOR_PLAN_CAPACITY;
    }
    PlanSlot& slot = owner.planSlots[static_cast<std::size_t>(slotIndex)];
    VnMonitorCapturePlan accepted{};
    const VnResult check = CopyValidatedPlan(plan, &accepted, &slot);
    if (check != VN_OK) {
        ReleaseFailedSlot(&slot);
        return check;
    }
    if (slot.storedMeasurementCount != 0 &&
        (accepted.streamKindMask & VN_MONITOR_STREAM_OPTIONAL_AUDIO) != 0) {
        try {
            auto derived = std::make_shared<DerivedDrainState>();
            const auto* previous = owner.activeSlot >= 0
                ? owner.planSlots[static_cast<std::size_t>(owner.activeSlot)].derivedState.get() : nullptr;
            if (!derived->Prepare(slot.measurements.data(), slot.storedMeasurementCount,
                                  owner.engine->config.mixRate.SampleRate(), previous)) {
                ReleaseFailedSlot(&slot);
                return VN_MONITOR_BUDGET_EXCEEDED;
            }
            slot.derivedState = std::move(derived);
        } catch (...) {
            ReleaseFailedSlot(&slot);
            return VN_MONITOR_BUDGET_EXCEEDED;
        }
    }
    if ((accepted.streamKindMask & VN_MONITOR_STREAM_OPTIONAL_AUDIO) != 0 &&
        !owner.engine->voices.PrepareMonitorPreChainBacking()) {
        ReleaseFailedSlot(&slot);
        return VN_MONITOR_BUDGET_EXCEEDED;
    }
    slot.occupancy = SlotOccupancy::Pending;
    slot.empty = PlanLooksEmpty(accepted);
    slot.quarantined = false;
    slot.plan = accepted;
    owner.pendingSlot = slotIndex;
    owner.pendingLeaseGeneration = owner.lease.generation;
    owner.pendingReadyFrame.store(
        owner.executeFrame.load(std::memory_order_relaxed) + 1,
        std::memory_order_release);
    owner.publishedPending.store(slotIndex, std::memory_order_release);
    WriteEffective(owner, outAccepted);
    return VN_OK;
}

VnResult StopCapture(VnMonitorSessionHandle handle, uint64_t leaseGeneration,
                     VnMonitorEffectiveCapturePlan* outResult) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (outResult == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outResult->structSize,
                         sizeof(VnMonitorEffectiveCapturePlan))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) {
        return VN_MONITOR_STALE_LEASE;
    }
    if (owner.pendingSlot >= 0 &&
        owner.pendingSlot != static_cast<std::int32_t>(kStopSlotIndex)) {
        PlanSlot& pending =
            owner.planSlots[static_cast<std::size_t>(owner.pendingSlot)];
        pending.derivedState.reset();
        pending.occupancy = SlotOccupancy::Free;
        pending.empty = true;
        FillEmptyPlan(&pending.plan);
        owner.pendingSlot = -1;
        owner.pendingLeaseGeneration = 0;
        owner.publishedPending.store(-1, std::memory_order_release);
        owner.pendingReadyFrame.store(0, std::memory_order_release);
    }
    if (owner.PlanIsEmptyLocked() && owner.pendingSlot < 0) {
        WriteEffective(owner, outResult);
        return VN_OK;
    }
    PlanSlot& stop = owner.planSlots[kStopSlotIndex];
    stop.occupancy = SlotOccupancy::Pending;
    stop.empty = true;
    stop.quarantined = false;
    FillEmptyPlan(&stop.plan);
    owner.pendingSlot = static_cast<std::int32_t>(kStopSlotIndex);
    owner.pendingLeaseGeneration = owner.lease.generation;
    owner.pendingReadyFrame.store(
        owner.executeFrame.load(std::memory_order_relaxed) + 1,
        std::memory_order_release);
    owner.publishedPending.store(owner.pendingSlot, std::memory_order_release);
    WriteEffective(owner, outResult);
    return VN_OK;
}

VnResult GetEffectiveCapturePlan(VnMonitorSessionHandle handle,
                                 uint64_t capturePlanGeneration,
                                 VnMonitorEffectiveCapturePlan* outPlan) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (outPlan == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outPlan->structSize,
                         sizeof(VnMonitorEffectiveCapturePlan))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    if (FindLive(owner, handle) == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (!GenerationMatchesLocked(owner, capturePlanGeneration)) {
        return VN_MONITOR_STALE_PLAN;
    }
    WriteEffectiveGeneration(owner, capturePlanGeneration, outPlan);
    return VN_OK;
}

VnResult StartLegacyRecording(VnMonitorSessionHandle handle, uint64_t leaseGeneration,
                        const VnMonitorRecordingDesc* desc) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (desc == nullptr || desc->utf8Path == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(desc->structSize, sizeof(VnMonitorRecordingDesc)) ||
        !ReservedZero(desc->reserved0)) {
        return VN_INVALID_ARG;
    }
    std::filesystem::path path;
    if (!TryMakeUtf8Path(desc->utf8Path, &path)) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if ((session->acceptedPermissionMask & kRecordPermission) == 0) {
        return VN_MONITOR_PERMISSION_DENIED;
    }
    if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) {
        return VN_MONITOR_STALE_LEASE;
    }
    if (session->executionFactVersion != 0) return VN_MONITOR_UNSUPPORTED;
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    if (owner.recorder.state == VN_MONITOR_RECORDING_STATE_ACTIVE ||
        owner.recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING) {
        return VN_IN_USE;
    }
    if (desc->fileByteBudget != 0 &&
        desc->fileByteBudget > VN_MONITOR_HARD_SESSION_FILE_BYTES) {
        return VN_MONITOR_BUDGET_EXCEEDED;
    }
    owner.recorder.ResetStopped();
    owner.recorder.path = path;
    owner.recorder.fileByteBudget =
        desc->fileByteBudget == 0 ? VN_MONITOR_DEFAULT_SESSION_FILE_BYTES
                                  : desc->fileByteBudget;
    owner.recorder.file.open(path, std::ios::binary | std::ios::trunc);
    if (!owner.recorder.file.is_open()) {
        return FailAndRemoveStartFileLocked(owner.recorder,
                                            VN_MONITOR_RECORDING_FAILED);
    }
    std::uint8_t header[kSessionFileHeaderPackedBytes];
    PackSessionFileHeader(header, 1, 1, 0, 0);
    owner.recorder.file.write(reinterpret_cast<const char*>(header),
                              sizeof(header));
    if (!owner.recorder.file.good()) {
        return FailAndRemoveStartFileLocked(owner.recorder,
                                            VN_MONITOR_RECORDING_FAILED);
    }
    if (!owner.internals[kInternalRecorder]) {
        return FailAndRemoveStartFileLocked(owner.recorder,
                                            VN_MONITOR_RECORDING_FAILED);
    }
    owner.recorder.queueEpoch = owner.nextEpoch++;
    owner.internals[kInternalRecorder]->Reset(
        owner.recorder.fileByteBudget, VN_MONITOR_DROP_RECORDER_QUEUE,
        owner.recorder.queueEpoch, 0);
    owner.recorder.fileBytes = sizeof(header);
    owner.recorder.state = VN_MONITOR_RECORDING_STATE_ACTIVE;
    if (!WriteRecorderChunkLocked(owner.recorder, VN_MONITOR_CHUNK_SESSION_HEADER,
                                  VN_MONITOR_CHUNK_REQUIRED, nullptr, 0)) {
        const VnResult failed = RecorderFailureResult(owner.recorder);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return failed;
    }
    if (owner.engine != nullptr && owner.engine->trace &&
        owner.engine->trace->Active()) {
        std::uint8_t correlation[kTraceCorrelationPackedBytes];
        const std::uint64_t executeFrame =
            owner.executeFrame.load(std::memory_order_relaxed);
        const std::uint64_t logicCommit = owner.engine->logicCommits.Current();
        PackTraceCorrelationPayload(correlation, 1, session->epoch, executeFrame,
                                    logicCommit, 0, 0, session->epoch);
        (void)TryWriteOptionalRecorderChunkLocked(
            owner.recorder, VN_MONITOR_CHUNK_TRACE_CORRELATION,
            VN_MONITOR_CHUNK_OPTIONAL, correlation, kTraceCorrelationPackedBytes);
        if (owner.recorder.state == VN_MONITOR_RECORDING_STATE_FAILED) {
            const VnResult failed = RecorderFailureResult(owner.recorder);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return failed;
        }
    }
    owner.internalEnabledMask.fetch_or(1u << kInternalRecorder,
                                       std::memory_order_release);
    return VN_OK;
}

VnResult StartRecording(VnMonitorSessionHandle handle, uint64_t leaseGeneration,
                        const VnMonitorRecordingDesc* desc) {
    const auto gate = GateControlEntry();
    if (gate != VN_OK) return gate;
    if (!desc || !desc->utf8Path || desc->structSize != sizeof(*desc) || desc->reserved0 != 0) return VN_INVALID_ARG;
    if (desc->fileByteBudget > VN_MONITOR_HARD_SESSION_FILE_BYTES) return VN_MONITOR_BUDGET_EXCEEDED;
    std::filesystem::path path;
    if (!TryMakeUtf8Path(desc->utf8Path, &path)) return VN_INVALID_ARG;
    auto& owner = Owner();
    if (IdentitySafeBoundary(owner)) RefreshIdentityBoundary();
    VnMonitorSessionDescV2 receiverDesc{};
    RecordingDescriptor descriptor{};
    {
        std::unique_lock<std::mutex> lock(owner.mutex);
        ExpireLeaseIfDueLocked(owner);
        CleanupRecordingReceiverLocked(owner);
        auto* session = FindLive(owner, handle);
        if (!session) return VN_MONITOR_STALE_SESSION;
        if (session->executionFactVersion == 0) {
            lock.unlock();
            return StartLegacyRecording(handle, leaseGeneration, desc);
        }
        if ((session->acceptedPermissionMask & kRecordPermission) == 0) return VN_MONITOR_PERMISSION_DENIED;
        if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) return VN_MONITOR_STALE_LEASE;
        if (!IdentitySafeBoundary(owner)) return VN_BACKPRESSURE;
        std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
        if (owner.recorder.state == VN_MONITOR_RECORDING_STATE_ACTIVE ||
            owner.recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING) return VN_IN_USE;
        descriptor.owner = handle;
        descriptor.ownerGeneration = session->generation;
        descriptor.ownerEpoch = session->epoch;
        descriptor.leaseGeneration = leaseGeneration;
        descriptor.engine = owner.engine->monitorRequests.EngineInstance();
        descriptor.recordingEpoch = owner.nextEpoch++;
        descriptor.schema = 2;
        descriptor.streams = session->acceptedStreamKindMask;
        descriptor.permissions = session->acceptedPermissionMask;
        descriptor.requestSymbols = session->requestSymbols;
        receiverDesc.structSize = sizeof(receiverDesc);
        receiverDesc.requestedStreamKindMask = descriptor.streams;
        receiverDesc.requestedPermissionMask = descriptor.permissions & ~(kRecordPermission | kCaptureControlPermission);
        receiverDesc.consumerQueueBytes = VN_MONITOR_DEFAULT_RECORDER_QUEUE_BYTES;
        receiverDesc.requestSymbols = session->requestSymbols;
        receiverDesc.requestRawAudio = session->requestRawAudio;
        receiverDesc.requestedExecutionFactVersion = descriptor.schema;
    }
    VnMonitorSessionHandle receiver = 0;
    const auto opened = OpenSessionWithPermissions(reinterpret_cast<const VnMonitorSessionDesc*>(&receiverDesc), false, 0, 0,
        &receiver, (descriptor.streams & VN_MONITOR_STREAM_EVENT) != 0, true);
    if (opened != VN_OK) return opened;
    void (*afterOpen)(void*) = nullptr;
    void* openContext = nullptr;
    {
        std::lock_guard<std::mutex> lock(owner.recorder.mutex);
        afterOpen = owner.recorder.afterReceiverOpenForTests;
        openContext = owner.recorder.receiverOpenContextForTests;
        owner.recorder.afterReceiverOpenForTests = nullptr;
        owner.recorder.receiverOpenContextForTests = nullptr;
    }
    if (afterOpen) afterOpen(openContext);
    std::lock_guard<std::mutex> ownerLock(owner.mutex);
    auto* session = FindLive(owner, handle);
    auto* subscription = FindLive(owner, receiver, true);
    const auto discard = [&](VnResult result) {
        if (subscription) RetireSessionLocked(owner, *subscription);
        return result;
    };
    if (!session || session->epoch != descriptor.ownerEpoch ||
        !IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) return discard(VN_MONITOR_STALE_LEASE);
    if (!subscription || subscription->acceptedStreamKindMask != descriptor.streams ||
        subscription->acceptedPermissionMask != receiverDesc.requestedPermissionMask ||
        subscription->executionFactVersion != descriptor.schema) return discard(VN_MONITOR_PERMISSION_DENIED);
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    auto& recorder = owner.recorder;
    recorder.ResetStopped();
    recorder.typed = true;
    recorder.receiver = receiver;
    descriptor.epoch = subscription->epoch;
    descriptor.startedNs = subscription->eventStartedNs;
    std::copy(subscription->supportedFactKinds.begin(), subscription->supportedFactKinds.end(), descriptor.supported);
    descriptor.controlStart = subscription->controlStart;
    std::copy(subscription->executionStarts.begin(), subscription->executionStarts.end(), descriptor.starts);
    descriptor.starts[2] = subscription->identityStart;
    descriptor.starts[3] = subscription->symbolStart;
    std::copy(subscription->ringStarts.begin(), subscription->ringStarts.end(), descriptor.ringStarts);
    std::copy(subscription->logStarts.begin(), subscription->logStarts.end(), descriptor.logStarts);
    std::copy(subscription->rawDropStarts.begin(), subscription->rawDropStarts.end(), descriptor.rawDropStarts);
    std::copy(subscription->unavailableStarts.begin(), subscription->unavailableStarts.end(), descriptor.unavailableStarts);
    if ((descriptor.streams & VN_MONITOR_STREAM_STATE) != 0) {
        descriptor.cutId = subscription->cut.id;
        descriptor.cutRevision = subscription->identity.begin.identityRevision;
        descriptor.cutFirst = subscription->cut.beginSourceSequence;
        descriptor.cutLast = subscription->cut.endSourceSequence;
    }
    recorder.descriptor = descriptor;
    recorder.fileByteBudget = desc->fileByteBudget == 0 ? VN_MONITOR_DEFAULT_SESSION_FILE_BYTES : desc->fileByteBudget;
    const auto fail = [&](VnResult reason) {
        FailAndRemoveStartFileLocked(recorder, reason);
        recorder.receiver = 0;
        return discard(reason);
    };
    if (!recorder.verifier.Begin(descriptor)) return fail(VN_MONITOR_INCOMPATIBLE);
    recorder.path = path;
    recorder.file.open(path, std::ios::binary | std::ios::trunc);
    if (!recorder.file.is_open()) return fail(VN_MONITOR_RECORDING_FAILED);
    std::uint8_t header[kSessionFileHeaderPackedBytes];
    PackSessionFileHeader(header, 1, descriptor.engine, 0, 0);
    recorder.file.write(reinterpret_cast<const char*>(header), sizeof(header));
    if (!recorder.file.good()) return fail(VN_MONITOR_RECORDING_FAILED);
    recorder.fileBytes = sizeof(header);
    recorder.queueEpoch = descriptor.epoch;
    recorder.state = VN_MONITOR_RECORDING_STATE_ACTIVE;
    std::uint8_t packed[kRecordingDescriptorBytes]{};
    ExecutionPayloadWriter write{packed};
    VisitRecordingDescriptor(write, descriptor);
    if (!WriteRecorderChunkLocked(recorder, VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR, VN_MONITOR_CHUNK_REQUIRED, packed, sizeof(packed)))
        return fail(RecorderFailureResult(recorder));
    owner.WakeDrain();
    return VN_OK;
}

VnResult StopRecording(VnMonitorSessionHandle handle, uint64_t leaseGeneration) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    RefreshIdentityBoundary();
    std::lock_guard<std::mutex> lock(owner.mutex);
    ExpireLeaseIfDueLocked(owner);
    Session* session = FindLive(owner, handle);
    if (session == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (!IsCurrentHolderLocked(owner, *session, handle, leaseGeneration)) {
        return VN_MONITOR_STALE_LEASE;
    }
    {
        std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
        if (owner.recorder.typed && (owner.recorder.descriptor.owner != handle ||
            owner.recorder.descriptor.leaseGeneration != leaseGeneration)) return VN_MONITOR_STALE_LEASE;
    }
    StopOwnedRecordingLocked(owner, handle, leaseGeneration, kLeaseReasonRelease);
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    if (owner.recorder.state == VN_MONITOR_RECORDING_STATE_ACTIVE) {
        owner.recorder.state = VN_MONITOR_RECORDING_STATE_STOPPING;
        owner.WakeDrain();
        return VN_OK;
    }
    if (owner.recorder.state == VN_MONITOR_RECORDING_STATE_STOPPING ||
        owner.recorder.state == VN_MONITOR_RECORDING_STATE_STOPPED) {
        return VN_OK;
    }
    return VN_MONITOR_RECORDING_FAILED;
}

VnResult GetRecordingStatus(VnMonitorSessionHandle handle,
                            VnMonitorRecordingStatus* outStatus) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (outStatus == nullptr) {
        return VN_INVALID_ARG;
    }
    if (!ValidStructSize(outStatus->structSize, sizeof(VnMonitorRecordingStatus))) {
        return VN_INVALID_ARG;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (FindLive(owner, handle) == nullptr) {
        return VN_MONITOR_STALE_SESSION;
    }
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    std::memset(outStatus, 0, sizeof(*outStatus));
    outStatus->structSize = sizeof(*outStatus);
    outStatus->state = owner.recorder.state;
    outStatus->fileBytes = owner.recorder.fileBytes;
    outStatus->lastCommittedChunk = owner.recorder.lastCommittedChunk;
    const auto* receiver = owner.recorder.typed ? FindLive(owner, owner.recorder.receiver, true) : nullptr;
    auto* queue = owner.recorder.typed ? (receiver ? receiver->queue.get() : nullptr) : owner.internals[kInternalRecorder].get();
    if (queue) {
        std::lock_guard<std::mutex> queueLock(queue->mutex);
        outStatus->queueBytes = queue->usedBytes;
    }
    outStatus->recoveryNeeded = owner.recorder.recoveryNeeded;
    outStatus->errorReason = owner.recorder.errorReason;
    return VN_OK;
}

bool PublishSyntheticTimeSeriesForTests(std::uint32_t count) {
    ProcessOwner& owner = Owner();
    EnsureCaptureResources(owner);
    std::uint8_t packed[kScratchBytes];
    bool all = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t sourceSequence =
            owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t size =
            PackSyntheticTimeSeries(packed, sizeof(packed), sourceSequence);
        if (size == 0) {
            all = false;
            continue;
        }
        StampPackedU64(packed, kPackedCapturePlanGenerationOffset,
                       owner.capturePlanGeneration);
        StampPackedU64(packed, kPackedExecuteFrameOffset, 0);
        if (!PublishPacked(packed, size)) {
            all = false;
        }
    }
    return all;
}

void DrainOnceForTests() {
    ProcessOwner& owner = Owner();
    if (owner.drainScratch.size() < PackedProducerSlotBytes()) {
        owner.drainScratch.assign(PackedProducerSlotBytes(), 0);
    }
    std::uint8_t* scratch = owner.drainScratch.data();
    const std::uint32_t scratchBytes =
        static_cast<std::uint32_t>(owner.drainScratch.size());
    while (DrainOneRound(owner, scratch, scratchBytes)) {
    }
}

void DrainRoundForTests() {
    auto& owner = Owner();
    DrainOneRound(owner, owner.drainScratch.data(), static_cast<std::uint32_t>(owner.drainScratch.size()));
}

void PumpRecorderForTests() { PumpRecorder(Owner()); }

VnMonitorSessionHandle RecordingReceiverForTests() {
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.recorder.mutex);
    return owner.recorder.receiver;
}

void SetRecordingQueueBudgetForTests(std::uint64_t bytes) {
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    std::lock_guard<std::mutex> recorderLock(owner.recorder.mutex);
    auto* session = FindLive(owner, owner.recorder.receiver, true);
    if (!session) return;
    std::lock_guard<std::mutex> queueLock(session->queue->mutex);
    session->queue->budgetBytes = bytes;
}

void DrainOneIdentityForTests() {
    auto& owner = Owner();
    DrainIdentityOne(owner, owner.drainScratch.data(), static_cast<std::uint32_t>(owner.drainScratch.size()));
}
void DrainOneTypedForTests() {
    auto& owner = Owner();
    DrainTypedOne(owner, owner.drainScratch.data(), static_cast<std::uint32_t>(owner.drainScratch.size()));
}
std::uint64_t SymbolPublishedForTests() { return Owner().symbolProducer.Published(); }
std::uint32_t SymbolRingCapacityForTests() { return Owner().symbolRing.Capacity(); }
std::uint64_t DeferredSymbolBytesForTests(VnMonitorSessionHandle handle) {
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    auto* session = FindLive(owner, handle);
    if (!session || !session->queue) return 0;
    std::lock_guard<std::mutex> queueLock(session->queue->mutex);
    std::uint64_t bytes = 0;
    for (const auto& record : session->queue->deferredSymbols) bytes += record.bytes.size();
    return bytes;
}
std::uint64_t ConsumerUsedBytesForTests(VnMonitorSessionHandle handle) {
    auto& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    auto* session = FindLive(owner, handle);
    if (!session || !session->queue) return 0;
    std::lock_guard<std::mutex> queueLock(session->queue->mutex);
    return session->queue->usedBytes;
}
void ExhaustSymbolSourcesForTests() {
    auto& owner = Owner();
    if (!owner.drainRunning.load(std::memory_order_acquire) && owner.symbolRing.Empty())
        owner.symbolProducer.Reset(UINT64_MAX);
}
void ExhaustObservationRevisionForTests() { Owner().observationRevision = UINT64_MAX; }
void DropIdentityRowForTests(std::int32_t row) { Owner().dropIdentityRowForTests = row; }
std::uint64_t IdentityRefreshesForTests() { return Owner().identityRefreshes; }
std::uint32_t IdentityRingCapacityForTests() { return Owner().identityRing.Capacity(); }
void ExhaustIdentitySourcesForTests(bool baseline) {
    auto& owner = Owner();
    // Only legal after StopDrainForTests and a complete manual drain.
    if (!owner.drainRunning.load(std::memory_order_acquire) && owner.identityRing.Empty()) {
        if (baseline) owner.nextIdentitySequence = 0;
        else owner.identityProducer.Reset(UINT64_MAX);
    }
}

void DrainOneProducerForTests(bool audioProducer) {
    auto& owner = Owner();
    auto& ring = audioProducer ? owner.producerRing : owner.logicRing;
    while (DrainOneFrom(owner, ring, owner.drainScratch.data(), static_cast<std::uint32_t>(owner.drainScratch.size()))) {}
}

VnResult EnableMonitorEndpoint(std::uint32_t profile) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    if (profile != VN_MONITOR_SECURITY_LOCAL_ACL &&
        profile != VN_MONITOR_SECURITY_LAN_MTLS) {
        return VN_INVALID_ARG;
    }
    if (profile == VN_MONITOR_SECURITY_LAN_MTLS) {
        return VN_MONITOR_UNSUPPORTED;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    CloseBoundPipe(owner.endpoint);
    owner.endpoint.profile = profile;
    owner.endpoint.epoch = 0;
    owner.endpoint.grantedPermissionMask = 0;
    if (profile == VN_MONITOR_SECURITY_LOCAL_ACL &&
        !BindLocalAclPipe(owner.endpoint)) {
        owner.endpoint = {};
        return VN_MONITOR_UNSUPPORTED;
    }
    return VN_OK;
}

void DisableMonitorEndpoint() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    CloseBoundPipe(owner.endpoint);
    owner.endpoint = {};
}

VnResult ConnectMonitorEndpoint() {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (!owner.endpoint.Enabled()) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (owner.endpoint.profile == VN_MONITOR_SECURITY_LOCAL_ACL &&
        owner.endpoint.boundHandle == 0) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (owner.endpoint.profile == VN_MONITOR_SECURITY_LOCAL_ACL &&
        !AcceptLocalAclClient(owner.endpoint)) {
        CloseBoundPipe(owner.endpoint);
        if (!BindLocalAclPipe(owner.endpoint)) {
            owner.endpoint.rejectsRemoteClients = false;
        }
        return VN_MONITOR_UNSUPPORTED;
    }
    owner.endpoint.epoch = owner.endpoint.nextEpoch++;
    return VN_OK;
}

void DisconnectMonitorEndpoint() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.endpoint.epoch = 0;
    owner.endpoint.grantedPermissionMask = 0;
    DisconnectRemoteControlLocked(owner);
    if (owner.endpoint.profile == VN_MONITOR_SECURITY_LOCAL_ACL) {
        CloseBoundPipe(owner.endpoint);
        if (!BindLocalAclPipe(owner.endpoint)) {
            owner.endpoint.rejectsRemoteClients = false;
        }
    }
}

constexpr std::uint16_t kMutationDirectPlay = 5;
constexpr std::uint16_t kMutationStop = 6;
constexpr std::uint16_t kMutationSetBusVolume = 8;
constexpr std::uint16_t kMutationStopAll = 92;

bool IsCommandMirrorMutation(std::uint16_t opcode) {
    switch (opcode) {
    case kMutationDirectPlay:
    case kMutationStop:
    case kMutationSetBusVolume:
    case kMutationStopAll:
        return true;
    default:
        return false;
    }
}

VnResult DispatchMonitorOpcode(std::uint16_t opcode, std::uint64_t epoch) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (!owner.endpoint.Enabled() || !owner.endpoint.Connected()) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (owner.endpoint.epoch != epoch) {
        return VN_MONITOR_STALE_SESSION;
    }
    if (IsCommandMirrorMutation(opcode)) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (opcode == 0) {
        return VN_OK;
    }
    return VN_MONITOR_UNSUPPORTED;
}

VnResult RejectMonitorMutation(std::uint16_t opcode) {
    ProcessOwner& owner = Owner();
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        epoch = owner.endpoint.epoch;
    }
    return DispatchMonitorOpcode(opcode, epoch);
}

VnResult CheckMonitorEpoch(std::uint64_t epoch, VnResult stale) {
    const VnResult gate = GateControlEntry();
    if (gate != VN_OK) {
        return gate;
    }
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (!owner.endpoint.Enabled() || !owner.endpoint.Connected() ||
        owner.endpoint.epoch != epoch) {
        return stale;
    }
    return VN_OK;
}

VnResult EnableMonitorEndpointForTests(std::uint32_t profile) {
    return EnableMonitorEndpoint(profile);
}

void DisableMonitorEndpointForTests() { DisableMonitorEndpoint(); }

std::string MonitorEndpointNameForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return owner.endpoint.Name();
}

int MonitorEndpointPortForTests() { return 0; }

VnResult ConnectMonitorEndpointForTests() {
#if defined(_WIN32)
    const std::string name = MonitorEndpointNameForTests();
    HANDLE client = INVALID_HANDLE_VALUE;
    if (!name.empty()) {
        client = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    }
    const VnResult connected = ConnectMonitorEndpoint();
    if (client != INVALID_HANDLE_VALUE) {
        CloseHandle(client);
    }
    return connected;
#else
    return ConnectMonitorEndpoint();
#endif
}

void DisconnectMonitorEndpointForTests() { DisconnectMonitorEndpoint(); }

std::uint64_t MonitorEndpointEpochForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return owner.endpoint.epoch;
}

VnResult SendMonitorOpcodeForTests(std::uint16_t opcode) {
    ProcessOwner& owner = Owner();
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        epoch = owner.endpoint.epoch;
    }
    return DispatchMonitorOpcode(opcode, epoch);
}

VnResult SendMonitorOpcodeWithEpochForTests(std::uint16_t opcode, std::uint64_t epoch) {
    return DispatchMonitorOpcode(opcode, epoch);
}

VnResult SendMonitorPollWithEpochForTests(std::uint64_t epoch) {
    return CheckMonitorEpoch(epoch, VN_MONITOR_STALE_SESSION);
}

VnResult SendMonitorLeaseWithEpochForTests(std::uint64_t epoch) {
    return CheckMonitorEpoch(epoch, VN_MONITOR_STALE_LEASE);
}

bool MonitorEndpointRejectsRemoteClientsForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return owner.endpoint.rejectsRemoteClients;
}

std::uint64_t MonitorEndpointLocalAclSidHashForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return owner.endpoint.localAclSidHash;
}

std::uint32_t MonitorEndpointGrantedPermissionMaskForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return owner.endpoint.grantedPermissionMask;
}

VnResult OfferSelfReportedPrincipalForTests(std::uint64_t,
                                            std::uint32_t) {
    return VN_MONITOR_PERMISSION_DENIED;
}

VnResult FailMonitorHandshakeForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (!owner.endpoint.Enabled()) {
        return VN_MONITOR_UNSUPPORTED;
    }
    owner.endpoint.epoch = 0;
    owner.endpoint.grantedPermissionMask = 0;
    return VN_MONITOR_INCOMPATIBLE;
}

VnResult GrantMonitorPermissionsForTests(std::uint32_t mask) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (!owner.endpoint.Enabled() || !owner.endpoint.Connected()) {
        return VN_MONITOR_UNSUPPORTED;
    }
    owner.endpoint.grantedPermissionMask = mask;
    return VN_OK;
}

bool MonitorEndpointEmitsRawAudioForTests() {
    return false;
}

std::uint32_t InternalQueuedCountForTests(std::uint32_t index) {
    ProcessOwner& owner = Owner();
    if (index >= kInternalConsumerCount || !owner.internals[index]) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(owner.internals[index]->mutex);
    return static_cast<std::uint32_t>(owner.internals[index]->records.size());
}

VnMonitorDropSummary InternalDropForTests(std::uint32_t index) {
    VnMonitorDropSummary summary{};
    summary.structSize = sizeof(summary);
    ProcessOwner& owner = Owner();
    if (index >= kInternalConsumerCount || !owner.internals[index]) {
        return summary;
    }
    std::lock_guard<std::mutex> lock(owner.internals[index]->mutex);
    const DropAccum& drop = owner.internals[index]->drops[0];
    summary.stage = drop.stage;
    summary.streamId = drop.streamId;
    summary.firstSourceSequence = drop.firstSourceSequence;
    summary.lastSourceSequence = drop.lastSourceSequence;
    summary.recordCount = drop.recordCount;
    summary.byteCount = drop.byteCount;
    summary.reason = drop.reason;
    return summary;
}

void SetFailInitAfterThreadForTests(bool fail) {
    Owner().failInitAfterThread = fail;
}

void SetRecorderWriteFailureForTests(bool fail) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.recorder.mutex);
    owner.recorder.failNextWriteForTests = fail;
}

void SetBeforeRecorderFailureCleanupForTests(void (*callback)(void*), void* context) {
    auto& recorder = Owner().recorder;
    std::lock_guard<std::mutex> lock(recorder.mutex);
    recorder.beforeFailureCleanupForTests = callback;
    recorder.failureCleanupContextForTests = context;
}

void SetAfterRecordingReceiverOpenForTests(void (*callback)(void*), void* context) {
    auto& recorder = Owner().recorder;
    std::lock_guard<std::mutex> lock(recorder.mutex);
    recorder.afterReceiverOpenForTests = callback;
    recorder.receiverOpenContextForTests = context;
}

void SetRecorderOptionalWriteFailureForTests(bool fail) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.recorder.mutex);
    owner.recorder.failNextOptionalWriteForTests = fail;
}

void SetRecorderOptionalRestoreFailureForTests(bool fail) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.recorder.mutex);
    owner.recorder.failOptionalRestoreForTests = fail;
}

std::uint32_t PublishMutexHitsForTests() {
    return Owner().publishMutexHits.load(std::memory_order_acquire);
}

void SetDrainHoldForTests(bool hold) {
    Owner().drainHold.store(hold, std::memory_order_release);
}

std::uint32_t DrainEnteredForTests() {
    return Owner().drainEntered.load(std::memory_order_acquire);
}

void ResetDrainEnteredForTests() {
    Owner().drainEntered.store(0, std::memory_order_release);
}

void SetFanoutHoldForTests(bool hold) {
    Owner().fanoutHold.store(hold, std::memory_order_release);
}

std::uint32_t FanoutEnteredForTests() {
    return Owner().fanoutEntered.load(std::memory_order_acquire);
}

void ResetFanoutEnteredForTests() {
    Owner().fanoutEntered.store(0, std::memory_order_release);
}

std::uint32_t PackedTimeSeriesBytesForTests() { return PackedTimeSeriesBytes(); }

bool PublishConformancePackedForTests(const std::uint8_t* bytes,
                                      std::uint32_t size) {
    if (bytes == nullptr || size == 0) {
        return false;
    }
    ProcessOwner& owner = Owner();
    if (owner.audioPackScratch.size() < PackedProducerSlotBytes()) {
        owner.audioPackScratch.assign(PackedProducerSlotBytes(), 0);
    }
    if (size > owner.audioPackScratch.size()) {
        return false;
    }
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(owner.audioPackScratch.data(), bytes, size);
    StampPackedU64(owner.audioPackScratch.data(), kPackedSourceSequenceOffset,
                   sourceSequence);
    StampPackedU64(owner.audioPackScratch.data(), kPackedDeliverySequenceOffset, 0);
    return PublishPacked(owner.audioPackScratch.data(), size);
}

VnMonitorDropSummary ProducerDropForTests() {
    const DropAccum snap = Owner().producerDrop.Snapshot();
    VnMonitorDropSummary summary{};
    summary.structSize = sizeof(summary);
    summary.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    summary.streamId = snap.streamId;
    summary.firstSourceSequence = snap.firstSourceSequence;
    summary.lastSourceSequence = snap.lastSourceSequence;
    summary.recordCount = snap.recordCount;
    summary.byteCount = snap.byteCount;
    summary.reason = snap.reason;
    return summary;
}

std::uint32_t ProducerRingCapacityForTests() {
    return Owner().producerRing.Capacity();
}

void StopDrainForTests() {
    ProcessOwner& owner = Owner();
    EnsureCaptureResources(owner);
    owner.drainPausedForTests = true;
    StopDrain(owner);
}

void StartDrainForTests() {
    ProcessOwner& owner = Owner();
    owner.drainPausedForTests = false;
    StartDrain(owner);
}

bool DrainRunningForTests() {
    return Owner().drainRunning.load(std::memory_order_acquire);
}

bool CaptureResourcesAllocatedForTests() {
    ProcessOwner& owner = Owner();
    return owner.captureResourcesReady || owner.producerRing.SlotBytes() != 0 ||
           owner.logicRing.SlotBytes() != 0 || owner.drainScratch.capacity() != 0 ||
           owner.audioPackScratch.capacity() != 0 || owner.interleaveScratch.capacity() != 0;
}

void SetFailCaptureResourcesForTests(bool fail) {
    Owner().failCaptureResourcesForTests = fail;
}

bool RecorderConsumerEnabledForTests() {
    return (Owner().internalEnabledMask.load(std::memory_order_acquire) &
            (1u << kInternalRecorder)) != 0;
}

void EnableInternalConsumersForTests(std::uint32_t mask) {
    ProcessOwner& owner = Owner();
    EnsureCaptureResources(owner);
    for (std::uint32_t i = 0; i < kInternalConsumerCount; ++i) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }
        if (!owner.internals[i]) {
            owner.internals[i] = std::make_unique<ConsumerQueue>();
        }
        owner.internals[i]->Reset(
            static_cast<std::uint64_t>(PackedProducerSlotBytes()) * 4u,
            i == kInternalRecorder ? VN_MONITOR_DROP_RECORDER_QUEUE
                                   : VN_MONITOR_DROP_REMOTE_TRANSPORT,
            owner.nextEpoch, 0);
    }
    owner.internalEnabledMask.store(mask, std::memory_order_release);
}

VnResult OpenMonitorEndpointSessionForTests(const VnMonitorSessionDesc* desc,
                                           VnMonitorSessionHandle* outHandle) {
    ProcessOwner& owner = Owner();
    std::uint32_t permissions = 0;
    std::uint64_t principalHash = 0;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        permissions = owner.endpoint.grantedPermissionMask;
        principalHash = owner.endpoint.localAclSidHash;
    }
    return OpenRemoteSession(desc, permissions, principalHash, outHandle);
}

void SetHostPrincipalForTests(bool authenticated, std::uint32_t observeMask) {
    t_hostPrincipal.authenticated = authenticated;
    t_hostPrincipal.observeMask = observeMask;
    t_hostPrincipalOverride = true;
}

void ClearHostPrincipalForTests() {
    t_hostPrincipal = DefaultHostPrincipal();
    t_hostPrincipalOverride = false;
}

bool PublishStateCutBeginForTests() {
    ProcessOwner& owner = Owner();
    std::uint8_t packed[kScratchBytes];
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t size =
        PackStateCutBegin(packed, sizeof(packed), sourceSequence, sourceSequence,
                          0);
    return size > 0 && PublishLogicPacked(packed, size);
}

bool PublishStateCutEndForTests() {
    ProcessOwner& owner = Owner();
    std::uint8_t packed[kScratchBytes];
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t size = PackStateCutEnd(
        packed, sizeof(packed), sourceSequence, sourceSequence, 0,
        VN_MONITOR_STATE_CUT_COMPLETE, 0);
    return size > 0 && PublishLogicPacked(packed, size);
}

void SetDropStateCutEndForTests(bool drop) {
    Owner().dropStateCutEnd = drop;
}

bool PublishEpochBoundaryForTests() {
    ProcessOwner& owner = Owner();
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        for (Session& session : owner.sessions) {
            if (session.live.load(std::memory_order_acquire)) {
                session.resyncRequired.store(true, std::memory_order_release);
            }
        }
    }
    std::uint8_t packed[kScratchBytes];
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t size = PackControlRecord(
        packed, sizeof(packed), sourceSequence,
        VN_MONITOR_RECORD_STREAM_BASELINE, VN_MONITOR_STREAM_TIME_SERIES,
        kSyntheticStreamId, owner.nextEpoch);
    return size > 0 && PublishLogicPacked(packed, size);
}

void OnAudioQuantum() {
    ProcessOwner& owner = Owner();
    owner.executeFrame.fetch_add(1, std::memory_order_relaxed);
    if (PendingReadyToActivate(owner)) {
        owner.WakeDrain();
    }
}

bool TimeSeriesLiveArmed() {
    return Owner().liveTimeSeriesCadenceHz.load(std::memory_order_relaxed) != 0;
}

bool AudioLiveArmed() {
    // Capture publication follows preparation of every monitor input buffer.
    return Owner().liveAudioArmed.load(std::memory_order_acquire) != 0;
}

bool LiveTapKindArmed(std::uint32_t tapKind) {
    if (tapKind >= 32u) {
        return false;
    }
    const ProcessOwner& owner = Owner();
    return owner.liveAudioArmed.load(std::memory_order_relaxed) != 0 &&
           (owner.liveTapKindMask.load(std::memory_order_relaxed) &
            (1u << tapKind)) != 0;
}

void PublishTimeSeriesQuantum(std::uint32_t callbackCpuUs,
                              std::uint32_t drainCpuUs,
                              std::uint32_t voicesCpuUs,
                              std::uint32_t busesCpuUs,
                              std::uint32_t arenaHighWaterBytes) {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)callbackCpuUs;
    (void)drainCpuUs;
    (void)voicesCpuUs;
    (void)busesCpuUs;
    (void)arenaHighWaterBytes;
    return;
#else
    ProcessOwner& owner = Owner();
    std::uint32_t cadenceHz = 0;
    std::uint64_t generation = 0;
    std::uint64_t executeFrame = 0;
    bool snapped = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t seq =
            owner.livePlanSeq.load(std::memory_order_acquire);
        if ((seq & 1ull) != 0) {
            continue;
        }
        cadenceHz =
            owner.liveTimeSeriesCadenceHz.load(std::memory_order_relaxed);
        generation =
            owner.liveCapturePlanGeneration.load(std::memory_order_relaxed);
        executeFrame = owner.executeFrame.load(std::memory_order_relaxed);
        if (owner.livePlanSeq.load(std::memory_order_acquire) == seq) {
            snapped = true;
            break;
        }
    }
    if (!snapped || cadenceHz == 0) {
        return;
    }
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    struct Series {
        std::uint64_t streamId;
        std::uint32_t metricId;
        std::uint32_t unitId;
        std::uint32_t valueBits;
    };
    const Series series[5] = {
        {kTimeSeriesCallbackCpuStreamId, kTimeSeriesMetricCallbackCpu,
         kTimeSeriesUnitMicroseconds, callbackCpuUs},
        {kTimeSeriesStageDrainCpuStreamId, kTimeSeriesMetricStageDrainCpu,
         kTimeSeriesUnitMicroseconds, drainCpuUs},
        {kTimeSeriesStageVoicesCpuStreamId, kTimeSeriesMetricStageVoicesCpu,
         kTimeSeriesUnitMicroseconds, voicesCpuUs},
        {kTimeSeriesStageBusesCpuStreamId, kTimeSeriesMetricStageBusesCpu,
         kTimeSeriesUnitMicroseconds, busesCpuUs},
        {kTimeSeriesArenaHighWaterStreamId, kTimeSeriesMetricArenaHighWater,
         kTimeSeriesUnitBytes, arenaHighWaterBytes},
    };
    std::uint8_t packed[PackedTimeSeriesBytes()];
    for (const Series& item : series) {
        const std::uint32_t size = PackNumericTimeSeries(
            packed, sizeof(packed), sourceSequence, item.streamId, item.metricId,
            item.unitId, cadenceHz, item.valueBits, generation, executeFrame);
        if (size == 0) {
            continue;
        }
        if (!owner.producerRing.TryPush(packed, size, 0, {}, monitor::RecordingRawStreams(packed, size))) {
            owner.producerDrop.Note(sourceSequence, size, VN_MONITOR_EVENT_DROP);
            continue;
        }
        owner.publishedCount.fetch_add(1, std::memory_order_relaxed);
    }
    owner.WakeDrain();
#endif
}

void PublishMasterAudioChunk(const float* interleaved, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t layout,
                             std::uint32_t sampleRate,
                             std::uint32_t masterGeneration,
                             std::uint64_t sourceFrameBegin) {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)interleaved;
    (void)frames;
    (void)channels;
    (void)layout;
    (void)sampleRate;
    (void)masterGeneration;
    (void)sourceFrameBegin;
    return;
#else
    if (interleaved == nullptr || frames == 0 || channels == 0 ||
        frames > VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES ||
        channels > kPackedAudioMaxChannels) {
        return;
    }
    if (!LiveTapKindArmed(VN_MONITOR_TAP_MASTER_OUTPUT)) {
        return;
    }
    ProcessOwner& owner = Owner();
    std::uint64_t generation = 0;
    std::uint64_t executeFrame = 0;
    std::uint32_t tapCount = 0;
    LiveTapSnapshot taps[kLiveTapSlots];
    if (!SnapLiveAudioPlan(owner, &generation, &executeFrame, &tapCount, taps)) {
        return;
    }
    if (owner.audioPackScratch.size() < PackedProducerSlotBytes()) {
        return;
    }
    std::uint8_t* packed = owner.audioPackScratch.data();
    const std::uint32_t packedBytes =
        static_cast<std::uint32_t>(owner.audioPackScratch.size());
    for (std::uint32_t i = 0; i < tapCount; ++i) {
        if (taps[i].kind != VN_MONITOR_TAP_MASTER_OUTPUT) {
            continue;
        }
        if (taps[i].selector == VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT) {
            if (!MaybeFollowSlotRebind(owner, i, &taps[i], 1,
                                       masterGeneration, generation, executeFrame,
                                       packed, packedBytes)) {
                continue;
            }
        } else if (taps[i].availability != 0) {
            continue;
        } else if (taps[i].ownerHandle != 1) {
            continue;
        } else if (!OwnerMatches(taps[i].selector, taps[i].ownerHandle,
                                 taps[i].ownerGeneration, 1,
                                 masterGeneration)) {
            MaybeExactInstanceStale(owner, i, &taps[i], generation, executeFrame,
                                    packed, packedBytes);
            continue;
        }
        PublishPackedAudioChunk(owner, packed, packedBytes, generation,
                                executeFrame, i, taps[i].descriptorId,
                                taps[i].descriptorGeneration, sampleRate,
                                channels, layout, sourceFrameBegin, interleaved,
                                frames);
    }
#endif
}

bool SnapLiveAudioPlan(ProcessOwner& owner, std::uint64_t* generation,
                       std::uint64_t* executeFrame, std::uint32_t* tapCount,
                       LiveTapSnapshot* taps) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t seq = owner.livePlanSeq.load(std::memory_order_acquire);
        if ((seq & 1ull) != 0) {
            continue;
        }
        const std::uint32_t armed =
            owner.liveAudioArmed.load(std::memory_order_relaxed);
        *generation =
            owner.liveCapturePlanGeneration.load(std::memory_order_relaxed);
        *executeFrame = owner.executeFrame.load(std::memory_order_relaxed);
        std::uint32_t count = owner.liveTapCount.load(std::memory_order_relaxed);
        if (count > kLiveTapSlots) {
            count = kLiveTapSlots;
        }
        *tapCount = count;
        if (taps != nullptr) {
            for (std::uint32_t i = 0; i < count; ++i) {
                taps[i].kind = owner.liveTapKind[i].load(std::memory_order_relaxed);
                taps[i].selector =
                    owner.liveTapSelector[i].load(std::memory_order_relaxed);
                taps[i].slotId = owner.liveTapSlotId[i].load(std::memory_order_relaxed);
                taps[i].followBound =
                    owner.liveTapFollowBound[i].load(std::memory_order_relaxed);
                taps[i].descriptorId =
                    owner.liveTapDescriptorId[i].load(std::memory_order_relaxed);
                taps[i].descriptorGeneration =
                    owner.liveTapDescriptorGeneration[i].load(
                        std::memory_order_relaxed);
                taps[i].ownerHandle =
                    owner.liveTapOwnerHandle[i].load(std::memory_order_relaxed);
                taps[i].ownerGeneration =
                    owner.liveTapOwnerGeneration[i].load(std::memory_order_relaxed);
                taps[i].availability =
                    owner.liveTapAvailability[i].load(std::memory_order_relaxed);
            }
        }
        if (owner.livePlanSeq.load(std::memory_order_acquire) == seq) {
            return armed != 0 && count != 0;
        }
    }
    return false;
}

bool OwnerMatches(std::uint32_t selector, std::uint64_t boundHandle,
                  std::uint64_t boundGeneration, std::uint64_t liveHandle,
                  std::uint64_t liveGeneration) {
    if (selector == VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT) {
        return true;
    }
    if (boundHandle == 0 || boundHandle != liveHandle) {
        return false;
    }
    if (boundGeneration == 0 || boundGeneration != liveGeneration) {
        return false;
    }
    return true;
}

void PublishPackedAudioChunk(ProcessOwner& owner, std::uint8_t* packed,
                             std::uint32_t packedBytes, std::uint64_t generation,
                             std::uint64_t executeFrame, std::uint32_t tapIndex,
                             std::uint64_t tapId, std::uint64_t tapGeneration,
                             std::uint32_t sampleRate, std::uint32_t channels,
                             std::uint32_t layout, std::uint64_t sourceFrameBegin,
                             const float* interleaved, std::uint32_t frames) {
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t slot =
        tapIndex < kLiveTapSlots ? tapIndex : 0u;
    const std::uint32_t lastRate =
        owner.lastAudioRate[slot].load(std::memory_order_acquire);
    const std::uint32_t lastChannels =
        owner.lastAudioChannels[slot].load(std::memory_order_relaxed);
    const std::uint32_t lastLayout =
        owner.lastAudioLayout[slot].load(std::memory_order_relaxed);
    if (AudioFormatChangesSegment(lastRate, lastChannels, lastLayout, sampleRate,
                                  channels, layout)) {
        owner.measurementSegmentGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    owner.lastAudioRate[slot].store(sampleRate, std::memory_order_release);
    owner.lastAudioChannels[slot].store(channels, std::memory_order_relaxed);
    owner.lastAudioLayout[slot].store(layout, std::memory_order_relaxed);
    const std::uint32_t size = PackAudioChunkRecord(
        packed, packedBytes, sourceSequence, generation, executeFrame, tapId,
        tapGeneration, kAudioSampleFormatF32, sampleRate, channels, layout,
        sourceFrameBegin, sourceFrameBegin + frames, interleaved, frames,
        owner.measurementSegmentGeneration.load(std::memory_order_acquire));
    if (size == 0) {
        return;
    }
    if (!owner.producerRing.TryPush(packed, size, 0, {}, monitor::RecordingRawStreams(packed, size))) {
        owner.producerDrop.Note(sourceSequence, size,
                                VN_MONITOR_DROP_SOURCE_PRODUCER);
        return;
    }
    owner.publishedCount.fetch_add(1, std::memory_order_relaxed);
    owner.WakeDrain();
}

bool MaybeFollowSlotRebind(ProcessOwner& owner, std::uint32_t tapIndex,
                           LiveTapSnapshot* tap, std::uint64_t liveHandle,
                           std::uint64_t liveGeneration,
                           std::uint64_t capturePlanGeneration,
                           std::uint64_t executeFrame, std::uint8_t* packed,
                           std::uint32_t packedBytes) {
    if (tap == nullptr || tap->selector != VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT) {
        return false;
    }
    if (tap->followBound == 0) {
        tap->ownerHandle = liveHandle;
        tap->ownerGeneration = liveGeneration;
        tap->followBound = 1;
        owner.liveTapOwnerHandle[tapIndex].store(liveHandle,
                                                 std::memory_order_relaxed);
        owner.liveTapOwnerGeneration[tapIndex].store(liveGeneration,
                                                     std::memory_order_relaxed);
        owner.liveTapFollowBound[tapIndex].store(1, std::memory_order_release);
        return true;
    }
    if (tap->ownerHandle == liveHandle && tap->ownerGeneration == liveGeneration) {
        return true;
    }
    owner.measurementSegmentGeneration.fetch_add(1, std::memory_order_acq_rel);
    const std::uint64_t newTapGeneration = tap->descriptorGeneration + 1;
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t size = PackTapBindingChanged(
        packed, packedBytes, sourceSequence, capturePlanGeneration, executeFrame,
        tap->descriptorId, tap->descriptorGeneration, tap->descriptorId,
        newTapGeneration);
    if (size == 0) {
        return false;
    }
    if (!owner.producerRing.TryPush(packed, size, 0, {}, monitor::RecordingRawStreams(packed, size))) {
        owner.producerDrop.Note(sourceSequence, size,
                                VN_MONITOR_DROP_SOURCE_PRODUCER);
        return false;
    }
    owner.publishedCount.fetch_add(1, std::memory_order_relaxed);
    owner.WakeDrain();
    tap->ownerHandle = liveHandle;
    tap->ownerGeneration = liveGeneration;
    tap->descriptorGeneration = newTapGeneration;
    owner.liveTapOwnerHandle[tapIndex].store(liveHandle, std::memory_order_relaxed);
    owner.liveTapOwnerGeneration[tapIndex].store(liveGeneration,
                                                 std::memory_order_relaxed);
    owner.liveTapDescriptorGeneration[tapIndex].store(newTapGeneration,
                                                      std::memory_order_release);
    return true;
}

bool MaybeExactInstanceStale(ProcessOwner& owner, std::uint32_t tapIndex,
                             LiveTapSnapshot* tap,
                             std::uint64_t capturePlanGeneration,
                             std::uint64_t executeFrame, std::uint8_t* packed,
                             std::uint32_t packedBytes) {
    if (tap == nullptr ||
        tap->selector != VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE ||
        tap->availability != 0) {
        return false;
    }
    const std::uint64_t sourceSequence =
        owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t size = PackTapBindingChanged(
        packed, packedBytes, sourceSequence, capturePlanGeneration, executeFrame,
        tap->descriptorId, tap->descriptorGeneration, tap->descriptorId,
        tap->descriptorGeneration, static_cast<std::uint32_t>(VN_MONITOR_STALE_TAP));
    if (size == 0) {
        return false;
    }
    if (!owner.producerRing.TryPush(packed, size, 0, {}, monitor::RecordingRawStreams(packed, size))) {
        owner.producerDrop.Note(sourceSequence, size,
                                VN_MONITOR_DROP_SOURCE_PRODUCER);
        return false;
    }
    owner.publishedCount.fetch_add(1, std::memory_order_relaxed);
    owner.WakeDrain();
    tap->availability = 1;
    owner.liveTapAvailability[tapIndex].store(1, std::memory_order_release);
    return true;
}

void NoteExactInstanceRetired(std::uint32_t tapKind, std::uint64_t ownerHandle,
                              std::uint64_t ownerGeneration) {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)tapKind;
    (void)ownerHandle;
    (void)ownerGeneration;
    return;
#else
    if (!LiveTapKindArmed(tapKind) || ownerHandle == 0) {
        return;
    }
    ProcessOwner& owner = Owner();
    std::uint64_t generation = 0;
    std::uint64_t executeFrame = 0;
    std::uint32_t tapCount = 0;
    LiveTapSnapshot taps[kLiveTapSlots];
    if (!SnapLiveAudioPlan(owner, &generation, &executeFrame, &tapCount, taps)) {
        return;
    }
    if (owner.audioPackScratch.size() < PackedProducerSlotBytes()) {
        return;
    }
    std::uint8_t* packed = owner.audioPackScratch.data();
    const std::uint32_t packedBytes =
        static_cast<std::uint32_t>(owner.audioPackScratch.size());
    for (std::uint32_t i = 0; i < tapCount; ++i) {
        if (taps[i].kind != tapKind ||
            taps[i].selector != VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE ||
            taps[i].availability != 0) {
            continue;
        }
        if (taps[i].ownerHandle != ownerHandle) {
            continue;
        }
        if (taps[i].ownerGeneration != 0 &&
            taps[i].ownerGeneration != ownerGeneration) {
            continue;
        }
        MaybeExactInstanceStale(owner, i, &taps[i], generation, executeFrame,
                                packed, packedBytes);
    }
#endif
}

void PublishPlanarAudioChunk(std::uint32_t tapKind, std::uint64_t ownerHandle,
                             std::uint64_t ownerGeneration, std::uint32_t slotId,
                             float* const* planes, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t offset,
                             std::uint32_t layout, std::uint32_t sampleRate,
                             std::uint64_t sourceFrameBegin) {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)tapKind;
    (void)ownerHandle;
    (void)ownerGeneration;
    (void)slotId;
    (void)planes;
    (void)frames;
    (void)channels;
    (void)offset;
    (void)layout;
    (void)sampleRate;
    (void)sourceFrameBegin;
    return;
#else
    if (planes == nullptr || frames == 0 || channels == 0 ||
        frames > VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES ||
        channels > kPackedAudioMaxChannels) {
        return;
    }
    if (!LiveTapKindArmed(tapKind)) {
        return;
    }
    ProcessOwner& owner = Owner();
    std::uint64_t generation = 0;
    std::uint64_t executeFrame = 0;
    std::uint32_t tapCount = 0;
    LiveTapSnapshot taps[kLiveTapSlots];
    if (!SnapLiveAudioPlan(owner, &generation, &executeFrame, &tapCount, taps)) {
        return;
    }
    if (owner.audioPackScratch.size() < PackedProducerSlotBytes() ||
        owner.interleaveScratch.size() < kInterleaveScratchFloats) {
        return;
    }
    bool matched = false;
    for (std::uint32_t i = 0; i < tapCount; ++i) {
        if (taps[i].kind == tapKind) {
            matched = true;
            break;
        }
    }
    if (!matched) {
        return;
    }
    float* interleaved = owner.interleaveScratch.data();
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            interleaved[i * channels + ch] = planes[ch][offset + i];
        }
    }
    std::uint8_t* packed = owner.audioPackScratch.data();
    const std::uint32_t packedBytes =
        static_cast<std::uint32_t>(owner.audioPackScratch.size());
    for (std::uint32_t i = 0; i < tapCount; ++i) {
        if (taps[i].kind != tapKind) {
            continue;
        }
        if (taps[i].selector == VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT) {
            if (taps[i].slotId != slotId) {
                continue;
            }
            if (!MaybeFollowSlotRebind(owner, i, &taps[i], ownerHandle,
                                       ownerGeneration, generation, executeFrame,
                                       packed, packedBytes)) {
                continue;
            }
        } else if (taps[i].availability != 0) {
            continue;
        } else if (taps[i].slotId != 0 && taps[i].slotId != slotId) {
            continue;
        } else if (taps[i].ownerHandle != ownerHandle) {
            continue;
        } else if (!OwnerMatches(taps[i].selector, taps[i].ownerHandle,
                                 taps[i].ownerGeneration, ownerHandle,
                                 ownerGeneration)) {
            MaybeExactInstanceStale(owner, i, &taps[i], generation, executeFrame,
                                    packed, packedBytes);
            continue;
        }
        PublishPackedAudioChunk(owner, packed, packedBytes, generation,
                                executeFrame, i, taps[i].descriptorId,
                                taps[i].descriptorGeneration, sampleRate,
                                channels, layout, sourceFrameBegin, interleaved,
                                frames);
    }
#endif
}

void AdvanceExecuteFrameForTests(std::uint64_t frames) {
    ProcessOwner& owner = Owner();
    owner.executeFrame.fetch_add(frames, std::memory_order_acq_rel);
    ProcessPendingOnDrain(owner);
}

void SetLeaseExpiryExecuteFrameForTests(std::uint64_t frame) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (owner.lease.held) {
        owner.lease.expiryExecuteFrame = frame;
        owner.leaseExpiryFrame.store(frame, std::memory_order_release);
    }
}

void HoldPlanAckForTests(bool hold) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.holdPlanAck = hold;
    if (!hold) {
        RetireLingeringLocked(owner);
    }
}

void SetAbandonedPlanWorkerForTests() {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (owner.activeSlot >= 0) {
        owner.planSlots[static_cast<std::size_t>(owner.activeSlot)].quarantined =
            true;
    }
}

void DropNextActivationRecordForTests(bool drop) {
    ProcessOwner& owner = Owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.dropNextActivation = drop;
}

std::uint64_t ExecuteFrameForTests() {
    return Owner().executeFrame.load(std::memory_order_acquire);
}

void FillProducerRingForTests() {
    ProcessOwner& owner = Owner();
    std::uint8_t packed[kScratchBytes];
    while (owner.producerRing.FreeSlots() > 0) {
        const std::uint64_t sourceSequence =
            owner.nextTimeSeriesSequence.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t size =
            PackSyntheticTimeSeries(packed, sizeof(packed), sourceSequence);
        if (size == 0 || !PublishPacked(packed, size)) {
            break;
        }
    }
}

} // namespace harken::monitor

namespace harken {
namespace {

std::uint32_t LifecycleEntityKind(MonitorLifecyclePhase phase,
                                  std::uint8_t playableKind) noexcept {
    switch (phase) {
    case MonitorLifecyclePhase::PreparedCandidate:
    case MonitorLifecyclePhase::PlayableStarted:
    case MonitorLifecyclePhase::StepExecuted:
    case MonitorLifecyclePhase::ActionTerminal:
    case MonitorLifecyclePhase::SelectionCommitted: return 0;
    case MonitorLifecyclePhase::EffectInstalled:
    case MonitorLifecyclePhase::EffectReleased:
        return VN_MONITOR_ENTITY_EFFECT;
    case MonitorLifecyclePhase::PlayableStart:
    case MonitorLifecyclePhase::PlayableLogicalComplete:
    case MonitorLifecyclePhase::PlayableAudibleRetiring:
    case MonitorLifecyclePhase::PlayableRetired:
    case MonitorLifecyclePhase::PlayableCancel:
        if (playableKind == static_cast<std::uint8_t>(PlayableKind::Container)) {
            return VN_MONITOR_ENTITY_CONTAINER;
        }
        return VN_MONITOR_ENTITY_PLAYABLE;
    case MonitorLifecyclePhase::ActionAccept:
    case MonitorLifecyclePhase::ActionStop:
    case MonitorLifecyclePhase::ActionResult:
    case MonitorLifecyclePhase::ActionReject:
        return VN_MONITOR_ENTITY_ACTION;
    case MonitorLifecyclePhase::QueueAdvance:
    case MonitorLifecyclePhase::QueueFail:
        return VN_MONITOR_ENTITY_QUEUE;
    case MonitorLifecyclePhase::TransitionStart:
    case MonitorLifecyclePhase::TransitionStop:
    case MonitorLifecyclePhase::TransitionReject:
        return VN_MONITOR_ENTITY_TRANSITION;
    case MonitorLifecyclePhase::VoicePending:
    case MonitorLifecyclePhase::VoiceLive:
    case MonitorLifecyclePhase::VoicePark:
    case MonitorLifecyclePhase::VoiceEnd:
        return VN_MONITOR_ENTITY_VOICE;
    }
    return VN_MONITOR_ENTITY_NONE;
}

std::uint32_t LifecycleEventClass(MonitorLifecyclePhase phase) noexcept {
    switch (phase) {
    case MonitorLifecyclePhase::PreparedCandidate:
    case MonitorLifecyclePhase::PlayableStarted:
    case MonitorLifecyclePhase::StepExecuted:
    case MonitorLifecyclePhase::ActionTerminal:
    case MonitorLifecyclePhase::SelectionCommitted: return 0;
    case MonitorLifecyclePhase::EffectInstalled:
        return VN_MONITOR_EVENT_START;
    case MonitorLifecyclePhase::EffectReleased:
        return VN_MONITOR_EVENT_STOP;
    case MonitorLifecyclePhase::PlayableStart:
    case MonitorLifecyclePhase::TransitionStart:
        return VN_MONITOR_EVENT_START;
    case MonitorLifecyclePhase::PlayableLogicalComplete:
    case MonitorLifecyclePhase::ActionResult:
        return VN_MONITOR_EVENT_RESULT;
    case MonitorLifecyclePhase::PlayableAudibleRetiring:
    case MonitorLifecyclePhase::PlayableRetired:
    case MonitorLifecyclePhase::ActionStop:
    case MonitorLifecyclePhase::TransitionStop:
        return VN_MONITOR_EVENT_STOP;
    case MonitorLifecyclePhase::PlayableCancel:
    case MonitorLifecyclePhase::ActionReject:
    case MonitorLifecyclePhase::QueueFail:
    case MonitorLifecyclePhase::TransitionReject:
        return VN_MONITOR_EVENT_REJECT;
    case MonitorLifecyclePhase::ActionAccept:
        return VN_MONITOR_EVENT_ACCEPT;
    case MonitorLifecyclePhase::QueueAdvance:
        return VN_MONITOR_EVENT_QUEUE;
    case MonitorLifecyclePhase::VoicePending:
    case MonitorLifecyclePhase::VoiceLive:
    case MonitorLifecyclePhase::VoicePark:
    case MonitorLifecyclePhase::VoiceEnd:
        return VN_MONITOR_EVENT_VOICE;
    }
    return 0;
}

bool LifecycleNeedsUpsert(MonitorLifecyclePhase phase) noexcept {
    return phase == MonitorLifecyclePhase::PlayableStart ||
           phase == MonitorLifecyclePhase::ActionAccept ||
           phase == MonitorLifecyclePhase::QueueAdvance ||
           phase == MonitorLifecyclePhase::TransitionStart ||
           phase == MonitorLifecyclePhase::VoicePending ||
           phase == MonitorLifecyclePhase::VoiceLive ||
           phase == MonitorLifecyclePhase::VoicePark;
}

bool LifecycleNeedsTombstone(MonitorLifecyclePhase phase) noexcept {
    return phase == MonitorLifecyclePhase::PlayableRetired ||
           phase == MonitorLifecyclePhase::VoiceEnd;
}

} // namespace

void ObserveMonitorCurrentWorld(std::uint64_t worldGeneration) noexcept {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)worldGeneration;
#else
    if (worldGeneration == 0) {
        return;
    }
    if (!monitor::LogicThreadReady()) {
        return;
    }
    monitor::Owner().currentWorldGeneration.store(worldGeneration,
                                                  std::memory_order_release);
#endif
}

void ObserveMonitorVariableResult(const MonitorVariableResultFacts& facts) noexcept {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)facts;
#else
    if (!monitor::ExecutionObservationEnabled() || !monitor::LogicThreadReady()) return;
    auto& engine = *monitor::Owner().engine;
    MonitorExecutionFact fact{};
    fact.engine = monitor::ExecutionEngineInstance();
    fact.kind = VN_MONITOR_FACT_VARIABLE_RESULT;
    fact.contentKind = VN_MONITOR_CONTENT_VARIABLE;
    fact.timestamp = LogTimestampNs();
    fact.result = facts.result;
    auto& body = fact.body.execution;
    body.identity.object = facts.variableId;
    body.identity.project = facts.projectId;
    body.identity.world = facts.worldGeneration;
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_TIMESTAMP);
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_RESULT);
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, facts.variableId != VnObjectId{});
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_PROJECT, facts.projectId != VnObjectId{});
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_WORLD, facts.worldGeneration != 0);
    MonitorControlCause cause{};
    cause.request = facts.request;
    cause.actionFiring = facts.actionFiring;
    cause.executionOrdinal = facts.executionOrdinal;
    monitor::CaptureExecutionRequests(fact, facts.request, cause, {});
    if (facts.actionFiring != 0) {
        body.relations.action = facts.actionFiring;
        monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_RELATED_ACTION);
    }
    const auto origin = facts.emitterOrigin.kind != MonitorEmitterKind::Unknown
        ? facts.emitterOrigin : MonitorEmitterOrigin{};
    const auto context = engine.emitters.Context(origin.emitter);
    monitor::CaptureFactEmitters(fact, origin,
        engine.monitorEmitters.CaptureBinding(origin, context.stateEmitter, context.spatialEmitter));
    auto& activity = body.activity;
    activity.logicCommit = facts.logicCommitSequence;
    activity.executeFrame = facts.actualFrame;
    std::memcpy(&activity.variableValueBits, &facts.value, sizeof(facts.value));
    activity.variableScope = facts.emitterScope ? 1u : 0u;
    activity.variableOutcome = facts.outcome;
    activity.variableTicket = facts.logicCommitSequence;
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_LOGIC_COMMIT, facts.logicCommitSequence != 0);
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_EXECUTE_FRAME, facts.outcome == VN_VARIABLE_OUTCOME_APPLIED);
    monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_ACTIVITY_VARIABLE);
    monitor::PublishExecutionFact(fact);
#endif
}

void ObserveMonitorLifecycle(MonitorLifecyclePhase phase,
                             std::uint64_t instanceHandle,
                             std::uint64_t instanceGeneration,
                             std::uint64_t worldGeneration,
                             VnObjectId stableId,
                             std::uint64_t relatedPlayableHandle,
                             std::uint64_t relatedActionHandle,
                             std::uint32_t reason,
                             std::uint8_t playableKind,
                             std::uint64_t occurrence,
                             const MonitorLifecycleFacts& facts) noexcept {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)phase;
    (void)instanceHandle;
    (void)instanceGeneration;
    (void)worldGeneration;
    (void)stableId;
    (void)relatedPlayableHandle;
    (void)relatedActionHandle;
    (void)reason;
    (void)playableKind;
    (void)occurrence;
    (void)facts;
    return;
#else
    monitor::ProcessOwner& owner = monitor::Owner();
    if (owner.observerCount.load(std::memory_order_acquire) == 0 &&
        owner.internalEnabledMask.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (instanceHandle == 0 || instanceGeneration == 0) {
        return;
    }
    if (!monitor::LogicThreadReady()) {
        return;
    }
    if (monitor::ExecutionObservationEnabled() && monitor::LifecycleFactKind(phase) != 0) {
        const auto context = owner.engine->emitters.Context(facts.emitterOrigin.emitter);
        const auto binding = owner.engine->monitorEmitters.CaptureBinding(facts.emitterOrigin,
            context.stateEmitter, context.spatialEmitter);
        auto fact = monitor::CaptureLifecycleFact(phase, monitor::ExecutionEngineInstance(),
            instanceHandle, instanceGeneration, worldGeneration, stableId, relatedActionHandle,
            reason, playableKind, facts, binding);
        fact.timestamp = LogTimestampNs();
        monitor::SetSourceField(fact, VN_MONITOR_FACT_FIELD_TIMESTAMP);
        monitor::PublishExecutionFact(fact);
    }
    if (!facts.publishLegacy || phase == MonitorLifecyclePhase::SelectionCommitted) return;
    monitor::ProcessOwner::LifecycleDedupeKey key{};
    key.instanceHandle = instanceHandle;
    key.instanceGeneration = instanceGeneration;
    key.occurrence = occurrence;
    key.phase = static_cast<std::uint8_t>(phase);
    if (owner.LifecycleAlreadySeen(key)) {
        return;
    }
    const std::uint32_t entityKind = LifecycleEntityKind(phase, playableKind);
    if (entityKind == VN_MONITOR_ENTITY_NONE) {
        return;
    }
    const std::uint32_t eventClass = LifecycleEventClass(phase);
    if (eventClass == 0) {
        return;
    }
    if (monitor::EventCatalogEnabled()) {
        VnMonitorEventDetail detail{};
        detail.structSize = sizeof(detail);
        detail.category = VN_MONITOR_EVENT_CATEGORY_LIFECYCLE;
        detail.code = static_cast<std::uint32_t>(phase);
        detail.level = eventClass == VN_MONITOR_EVENT_REJECT ? VN_LOG_WARN : VN_LOG_INFO;
        detail.result = static_cast<std::int32_t>(reason);
        detail.timestampNs = LogTimestampNs();
        detail.arg0 = relatedPlayableHandle;
        detail.arg1 = relatedActionHandle;
        monitor::PublishEventDetail(detail, entityKind, stableId, facts.projectId, worldGeneration, instanceHandle,
                                    instanceGeneration, facts.programSemanticFingerprint);
        owner.RememberLifecycleKey(key);
    }
    const bool attributable =
        facts.arrangingInstance != 0 && facts.logicCommitSequence != 0;
    monitor::LifecycleEnvelope envelope{};
    envelope.sourceSequence = monitor::NextLogicSourceSequence();
    envelope.worldGeneration = worldGeneration;
    envelope.executeFrame = monitor::CurrentExecuteFrame();
    envelope.stableId = stableId;
    envelope.instanceHandle = instanceHandle;
    envelope.instanceGeneration = instanceGeneration;
    envelope.playableInstanceHandle =
        entityKind == VN_MONITOR_ENTITY_PLAYABLE
            ? (relatedPlayableHandle != 0 ? relatedPlayableHandle
                                          : instanceHandle)
            : relatedPlayableHandle;
    envelope.actionInstanceHandle = relatedActionHandle;
    envelope.entityKind = entityKind;
    envelope.reason = reason;
    envelope.programSemanticFingerprint = facts.programSemanticFingerprint;
    envelope.runtimeLineageFingerprint = facts.runtimeLineageFingerprint;
    envelope.arrangingInstance =
        attributable ? facts.arrangingInstance : 0;
    envelope.occurrenceOrdinal =
        entityKind == VN_MONITOR_ENTITY_VOICE
            ? 0u
            : (occurrence <= 0xffffffffull
                   ? static_cast<std::uint32_t>(occurrence)
                   : 0u);
    envelope.logicCommitSequence =
        attributable ? facts.logicCommitSequence : 0;
    envelope.volumeDb = facts.volumeDb;
    envelope.volumeKnown = facts.volumeKnown;
    envelope.notifyType = facts.notifyType;
    const bool needsUpsert = LifecycleNeedsUpsert(phase);
    const bool needsTombstone = LifecycleNeedsTombstone(phase);
    const std::uint32_t requiredSlots =
        1u + (needsUpsert ? 1u : 0u) + (needsTombstone ? 1u : 0u);
    std::uint8_t packed[monitor::kScratchBytes];
    const std::uint32_t eventSize =
        monitor::PackLifecycleEvent(packed, sizeof(packed), envelope, eventClass);
    if (eventSize == 0) {
        return;
    }
    std::uint8_t followPacked[monitor::kScratchBytes];
    std::uint32_t followSize = 0;
    if (needsUpsert || needsTombstone) {
        envelope.sourceSequence = monitor::NextLogicSourceSequence();
        followSize = needsUpsert
                         ? monitor::PackLifecycleUpsert(
                               followPacked, sizeof(followPacked), envelope)
                         : monitor::PackLifecycleTombstone(
                               followPacked, sizeof(followPacked), envelope);
        if (followSize == 0) {
            return;
        }
    }
    if (owner.logicRing.FreeSlots() < requiredSlots) {
        return;
    }
    if (!monitor::PublishLogicPacked(packed, eventSize)) {
        return;
    }
    if (followSize != 0 &&
        !monitor::PublishLogicPacked(followPacked, followSize)) {
        return;
    }
    owner.RememberLifecycleKey(key);
#endif
}

namespace {

enum class MonitorObjectKind : std::uint8_t {
    EffectUpsert = 1,
    BusUpsert = 2,
    EffectRelease = 3,
    DspGap = 4
};

// Private Hub dedupe slots. Not MonitorLifecyclePhase and not public ABI.
enum class MonitorObjectPhase : std::uint8_t {
    EffectUpsert = 200,
    BusUpsert = 201,
    EffectRelease = 202,
    DspGap = 203
};

void PublishObjectEnvelope(MonitorObjectKind kind,
                           const monitor::LifecycleEnvelope& seed,
                           MonitorObjectPhase occurrencePhase) noexcept {
#if defined(HARKEN_MONITOR_HUB_ABSENT)
    (void)kind;
    (void)seed;
    (void)occurrencePhase;
#else
    monitor::ProcessOwner& owner = monitor::Owner();
    if (owner.observerCount.load(std::memory_order_acquire) == 0 &&
        owner.internalEnabledMask.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (kind != MonitorObjectKind::DspGap) {
        if (seed.instanceHandle == 0 || seed.instanceGeneration == 0) {
            return;
        }
    }
    if (!monitor::LogicThreadReady()) {
        return;
    }
    monitor::ProcessOwner::LifecycleDedupeKey key{};
    key.instanceHandle = seed.instanceHandle;
    key.instanceGeneration = seed.instanceGeneration;
    key.occurrence = 0;
    key.phase = static_cast<std::uint8_t>(occurrencePhase);
    const bool replaceUpsert = kind == MonitorObjectKind::EffectUpsert ||
                               kind == MonitorObjectKind::BusUpsert;
    if (kind != MonitorObjectKind::DspGap && !replaceUpsert &&
        owner.LifecycleAlreadySeen(key)) {
        return;
    }
    monitor::LifecycleEnvelope envelope = seed;
    envelope.executeFrame = monitor::CurrentExecuteFrame();
    std::uint8_t packed[monitor::kScratchBytes];
    std::uint32_t size = 0;
    envelope.sourceSequence = monitor::NextLogicSourceSequence();
    if (kind == MonitorObjectKind::EffectRelease) {
        size = monitor::PackLifecycleTombstone(packed, sizeof(packed), envelope);
    } else if (kind == MonitorObjectKind::DspGap) {
        size = monitor::PackLifecycleEvent(packed, sizeof(packed), envelope,
                                           VN_MONITOR_EVENT_DROP);
    } else {
        size = monitor::PackLifecycleUpsert(packed, sizeof(packed), envelope);
    }
    if (size == 0) {
        return;
    }
    if (kind == MonitorObjectKind::DspGap && owner.logicRing.FreeSlots() < 1u) {
        if (owner.drainRunning.load(std::memory_order_acquire) &&
            !owner.drainStop.load(std::memory_order_acquire)) {
            owner.WakeDrain();
            for (int spin = 0; spin < 64 && owner.logicRing.FreeSlots() < 1u;
                 ++spin) {
                std::this_thread::yield();
            }
        } else {
            if (owner.drainScratch.size() < monitor::PackedProducerSlotBytes()) {
                owner.drainScratch.assign(monitor::PackedProducerSlotBytes(), 0);
            }
            (void)monitor::DrainOneFrom(
                owner, owner.logicRing, owner.drainScratch.data(),
                static_cast<std::uint32_t>(owner.drainScratch.size()));
        }
    }
    if (owner.logicRing.FreeSlots() < 1u) {
        return;
    }
    if (!monitor::PublishLogicPacked(packed, size)) {
        return;
    }
    if (kind != MonitorObjectKind::DspGap) {
        owner.RememberLifecycleKey(key);
    }
#endif
}

} // namespace

void ObserveMonitorEffect(std::uint64_t instanceHandle,
                          std::uint64_t instanceGeneration,
                          VnObjectId stableId, std::uint32_t reason,
                          const MonitorEffectFacts& facts) noexcept {
    monitor::LifecycleEnvelope envelope{};
    envelope.instanceHandle = instanceHandle;
    envelope.instanceGeneration = instanceGeneration;
    envelope.stableId = stableId;
    envelope.entityKind = VN_MONITOR_ENTITY_EFFECT;
    envelope.reason = reason;
    envelope.bypass01 = facts.bypass01;
    envelope.latencyFrames = facts.latencyFrames;
    envelope.tailFrames = facts.tailFrames;
    // latencyFrames has no measured source this milestone. Keep UNSUPPORTED on
    // the shared flag so a zero latency is not read as NUMERIC. Tail may still
    // be a known number on the same envelope.
    envelope.validityFlags = VN_MONITOR_VALID_UNSUPPORTED;
    if (facts.tailKnown) {
        envelope.validityFlags |= VN_MONITOR_VALID_NUMERIC;
    }
    PublishObjectEnvelope(MonitorObjectKind::EffectUpsert, envelope,
                          MonitorObjectPhase::EffectUpsert);
}

void ObserveMonitorBus(std::uint64_t instanceHandle,
                       std::uint64_t instanceGeneration, VnObjectId stableId,
                       const MonitorBusFacts& facts) noexcept {
    monitor::LifecycleEnvelope envelope{};
    envelope.instanceHandle = instanceHandle;
    envelope.instanceGeneration = instanceGeneration;
    envelope.stableId = stableId;
    envelope.entityKind = VN_MONITOR_ENTITY_BUS;
    envelope.mute01 = facts.mute01;
    envelope.solo01 = facts.solo01;
    envelope.routeBusId = facts.routeBusId;
    PublishObjectEnvelope(MonitorObjectKind::BusUpsert, envelope,
                          MonitorObjectPhase::BusUpsert);
}

void ObserveMonitorEffectRelease(std::uint64_t instanceHandle,
                                 std::uint64_t instanceGeneration,
                                 VnObjectId stableId,
                                 std::uint32_t reason) noexcept {
    monitor::LifecycleEnvelope envelope{};
    envelope.instanceHandle = instanceHandle;
    envelope.instanceGeneration = instanceGeneration;
    envelope.stableId = stableId;
    envelope.entityKind = VN_MONITOR_ENTITY_EFFECT;
    envelope.reason = reason;
    PublishObjectEnvelope(MonitorObjectKind::EffectRelease, envelope,
                          MonitorObjectPhase::EffectRelease);
}

void ObserveMonitorSelectionUnavailable() noexcept {
#if !defined(HARKEN_MONITOR_HUB_ABSENT)
    if (!monitor::ExecutionObservationEnabled() || !monitor::LogicThreadReady()) return;
    auto& owner = monitor::Owner();
    owner.executionProducers[0].DeclareDrop(owner.engine->monitorRequests.EngineInstance());
    owner.WakeDrain();
#endif
}

void ObserveMonitorDspGap() noexcept {
    monitor::LifecycleEnvelope envelope{};
    envelope.entityKind = VN_MONITOR_ENTITY_NONE;
    envelope.reason = VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE;
    envelope.validityFlags = VN_MONITOR_VALID_UNSUPPORTED;
    PublishObjectEnvelope(MonitorObjectKind::DspGap, envelope,
                          MonitorObjectPhase::DspGap);
}

} // namespace harken
