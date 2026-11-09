#include "harken/media/StreamManager.h"

#include "harken/core/MediaContentRevision.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/LogRing.h"
#include "harken/core/Utf8Path.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Keep the WinSDK alias from rewriting the StreamManager method definition.
#ifdef ReplaceFile
#undef ReplaceFile
#endif
#endif

namespace harken {

namespace {

std::atomic<StreamRequestFault> g_streamRequestFault{StreamRequestFault::None};
std::atomic<bool> g_streamWorkerStallRelease{false};
std::atomic<std::uint32_t> g_streamWorkerStallEntries{0};

// How often the streaming thread wakes on its own to top rings up when
// nothing kicked it. Two milliseconds keeps worst-case refill latency far
// inside what a prebuffered ring absorbs, at negligible idle cost.
constexpr std::chrono::milliseconds kServiceInterval{2};

// Registration-time sanity bound on a file's declared sample rate: no
// real recording exceeds 1 MHz, and bounding it keeps downstream frame
// math (prebuffer spans, resample ratios) inside 64-bit headroom.
constexpr std::uint32_t kMaxSaneSampleRate = 1'000'000;


std::uint64_t RoundUpPow2(std::uint64_t v) {
    std::uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

// Mirrors the pack loader's TOC validation posture: every bound is checked
// in an order that cannot wrap.
bool ValidateStreamEntry(const StreamHeader& header,
                            const StreamTocEntry& e,
                            std::uint64_t fileBytes) {
    if (e.sampleFormat != kStreamSampleFormatF32Planar) {
        return false;
    }
    if ((e.flags & ~kStreamKnownFlagsMask) != 0) {
        return false;
    }
    if (e.sampleRate == 0 || e.sampleRate > kMaxSaneSampleRate ||
        e.channelCount == 0 || e.channelCount > kMaxChannels ||
        e.frameCount == 0) {
        return false;
    }
    // chunkCount must be exactly the count the chunk grid implies; the
    // explicit field exists to be cross-checked, not trusted.
    if (e.frameCount >
        static_cast<std::uint64_t>(header.chunkFrames) * 0xffffffffull) {
        return false; // chunk index space exceeded
    }
    if (e.chunkCount == 0 ||
        e.chunkCount != StreamChunkCount(e.frameCount,
                                            header.chunkFrames)) {
        return false;
    }
    const std::uint64_t checksumBytes =
        static_cast<std::uint64_t>(e.chunkCount) *
        kStreamChunkChecksumBytes;
    const std::uint64_t tocEnd =
        static_cast<std::uint64_t>(header.tocOffset) +
        kStreamTocEntryBytes;
    if (e.chunkChecksumOffset < tocEnd ||
        e.chunkChecksumOffset > fileBytes ||
        checksumBytes > fileBytes - e.chunkChecksumOffset) {
        return false;
    }
    if (e.payloadOffset % kStreamPayloadAlign != 0) {
        return false;
    }
    if (e.payloadOffset < e.chunkChecksumOffset + checksumBytes) {
        return false;
    }
    // Fixed-size framing: the payload is exactly chunkCount full chunks,
    // the last one zero-padded.
    const std::uint64_t chunkBytes =
        StreamChunkBytes(header.chunkFrames, e.channelCount);
    if (chunkBytes == 0 ||
        e.chunkCount > UINT64_MAX / chunkBytes) {
        return false;
    }
    if (e.payloadBytes !=
        static_cast<std::uint64_t>(e.chunkCount) * chunkBytes) {
        return false;
    }
    if (e.payloadOffset > fileBytes ||
        e.payloadBytes > fileBytes - e.payloadOffset) {
        return false;
    }
    if ((e.flags & kStreamFlagLoopable) != 0) {
        const std::uint64_t loopEnd =
            e.loopEndFrame == 0 ? e.frameCount : e.loopEndFrame;
        if (loopEnd > e.frameCount || e.loopStartFrame >= loopEnd) {
            return false;
        }
    } else if (e.loopStartFrame != 0 || e.loopEndFrame != 0) {
        return false;
    }
    return true;
}

} // namespace

void SetStreamRequestFaultForTests(StreamRequestFault fault) noexcept {
    g_streamRequestFault.store(fault, std::memory_order_release);
}

StreamRequestFault StreamRequestFaultForTests() noexcept {
    return g_streamRequestFault.load(std::memory_order_acquire);
}

void SetStreamWorkerStallReleaseForTests(bool release) noexcept {
    g_streamWorkerStallRelease.store(release, std::memory_order_release);
}

std::uint32_t StreamWorkerStallEntriesForTests() noexcept {
    return g_streamWorkerStallEntries.load(std::memory_order_acquire);
}

std::size_t StreamManager::PendingRequestCountForTest() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests.size() + static_cast<std::size_t>(std::count_if(
        m_sourceRequests.begin(), m_sourceRequests.end(), [](const auto& request) {
            return request && !request->discard && request->state == PreparationState::Preparing;
        }));
}

StreamManager::StreamManager(const EngineConfig& config)
    : m_ringBytes(static_cast<std::uint32_t>(
          RoundUpPow2(config.streamRingBytes))),
      m_prebufferMs(config.prebufferMs),
      m_slots(config.maxStreams) {
    const auto storageBytes = static_cast<std::uint64_t>(config.maxStreams) * m_ringBytes;
    if (storageBytes > UINT32_MAX || !m_ringStorage.Reserve(static_cast<std::uint32_t>(storageBytes)))
        throw std::bad_alloc();
    for (std::uint32_t slot = 0; slot < config.maxStreams; ++slot)
        m_slots[slot].ring.Wire(m_ringStorage.data() + static_cast<std::size_t>(slot) * m_ringBytes, m_ringBytes);
    try {
        // Source validation/cleanup cannot occupy slot prebuffer execution,
        // and neither preparation domain borrows a steady refill worker.
        const auto lanes = config.maxStreams == 0 ? 1u : 2u + std::min(2u, config.maxStreams);
        for (unsigned i = 0; i < lanes; ++i) m_ioLanes.push_back(std::make_unique<IoLane>());
        for (unsigned i = 0; i < lanes; ++i)
            m_ioLanes[i]->thread = std::thread(&StreamManager::IoMain, this, i);
        m_thread = std::thread(&StreamManager::ThreadMain, this);
    } catch (...) { StopIo(); throw; }

    // Startup handshake. Synchronous callers require a running manager, so a
    // thread that died on the way up must refuse initialization. Wait until it has published a verdict; it always
    // publishes one, because both transitions are an atomic store followed by
    // a notify and neither can throw. The wait re-checks on a bounded interval
    // rather than trusting the notify, so a wake-up lost to the terminal path
    // (which may not be able to hold the mutex) still resolves.
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_workerState.load(std::memory_order_acquire) ==
               StreamWorkerState::Starting) {
            m_stateChanged.wait_for(lock, kServiceInterval);
        }
    }
    if (m_workerState.load(std::memory_order_acquire) ==
        StreamWorkerState::Failed) {
        // Join before throwing: destroying a joinable thread terminates. The
        // throw reaches Vn_Init's construction guard, which reports
        // VN_INVALID_CONFIG -- no half-alive engine whose stream calls hang.
        m_thread.join();
        StopIo();
        throw std::runtime_error("Harken streaming thread failed to start");
    }
}

StreamManager::~StreamManager() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_wake.notify_all();
    m_thread.join();
    StopIo();
    // The streaming thread is gone for good, so nothing can serve a request
    // that raced the shutdown in. Releasing them here keeps the destructor from
    // leaving a caller blocked on a completion that can never be posted.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        FailQueuedRequestsLocked();
    }
    m_done.notify_all();
}

void StreamManager::PublishWorkerState(StreamWorkerState state) noexcept {
    m_workerState.store(state, std::memory_order_release);
    // Both the startup handshake and the request waiters gate on this state.
    m_stateChanged.notify_all();
    m_done.notify_all();
}

void StreamManager::FailQueuedRequestsLocked() noexcept {
    for (Request* request : m_requests) {
        request->result = VN_DEVICE_ERROR;
        request->done = true;
    }
    m_requests.clear();
}

void StreamManager::EndActiveStreamsOnFailure() noexcept {
    for (StreamSlot& slot : m_slots) {
        if (slot.ring.state.load(std::memory_order_acquire) ==
            StreamSlotState::Active) {
            // Terminal publication goes through the ring's own arbitration, so a
            // park the audio thread already claimed still wins and the reason is
            // recorded as producer failure rather than a content outcome.
            // The consumer needs no new state either way: it drains the
            // buffered remainder and then the source has ended.
            slot.ring.MarkEndOfData(StreamTerminalReason::ProducerFailure);
        }
    }
}

void StreamManager::HandleTerminalWorkerFailure() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    EndActiveStreamsOnFailure();
    for (auto& lane : m_ioLanes) lane->cancelled.store(true, std::memory_order_release);
    for (auto& slot : m_slots) slot.io.cancelled.store(true, std::memory_order_release);
    for (auto& request : m_sourceRequests) {
        if (!request) continue;
        request->cancelled.store(true, std::memory_order_release);
        if (request->state == PreparationState::Preparing || request->state == PreparationState::Ready) {
            request->state = PreparationState::Failed;
            if (request->ioLane < 0) request->result = VN_DEVICE_ERROR;
        }
    }
    FailQueuedRequestsLocked();
    for (auto* request : m_batch) { request->result = VN_DEVICE_ERROR; request->done = true; }
    m_batch.clear();
    PublishWorkerState(StreamWorkerState::Failed);
}

void StreamManager::ReclaimAfterWorkerFailure() {
    if (WorkerState() != StreamWorkerState::Failed) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    EndActiveStreamsOnFailure();
    SettleIo();
    DetachRetiredSlots();
    CloseDrainedRetiringFiles();
    DispatchIo(); // Failed permits cleanup only.
}

bool StreamManager::ConsumeWorkerFailureForLog() noexcept {
    if (m_workerFailureLogged) {
        return false;
    }
    if (m_workerState.load(std::memory_order_acquire) !=
        StreamWorkerState::Failed) {
        return false;
    }
    m_workerFailureLogged = true;
    return true;
}

VnResult StreamManager::RegisterFile(VnSoundId soundId, const char* utf8Path) {
    return RegisterSourceAndWait(soundId, utf8Path, false);
}
VnResult StreamManager::UnregisterFile(VnSoundId soundId) {
    Request request;
    request.kind = Request::Kind::Unregister;
    request.soundId = soundId;
    return PostAndWait(request);
}
VnResult StreamManager::ReplaceFile(VnSoundId soundId, const char* utf8Path) {
    return RegisterSourceAndWait(soundId, utf8Path, true);
}

bool StreamManager::FindFile(VnSoundId soundId, StreamFacts* out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        const StreamFile* file = m_files[i].get();
        if (file == nullptr || file->retiring || file->packPrivate ||
            file->soundId != soundId) {
            continue;
        }
        out->fileIndex = static_cast<std::uint32_t>(i);
        out->sampleRate = file->toc.sampleRate;
        out->channelCount = file->toc.channelCount;
        out->frameCount = file->toc.frameCount;
        out->contentRevision = file->contentRevision;
        out->sourceGeneration = file->sourceGeneration;
        out->flags = file->toc.flags;
        out->loopStartFrame = file->toc.loopStartFrame;
        out->loopEndFrame = file->toc.loopEndFrame;
        return true;
    }
    return false;
}

VnResult StreamManager::ResolvePackSource(
    const std::shared_ptr<const PackMediaRange>& range, StreamFacts* out) {
    if (out == nullptr || range == nullptr || range->source == nullptr ||
        range->channelCount == 0 || range->channelCount > kMaxChannels ||
        range->chunkFrames == 0 || range->chunkFrames > kMaxStreamChunkFrames ||
        range->chunkChecksums.size() != PackMediaChunkCount(range->frameCount, range->chunkFrames))
        return VN_INVALID_ARG;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || WorkerState() != StreamWorkerState::Ready) return VN_DEVICE_ERROR;
    const auto fill = [&](std::size_t index, const StreamFile& file) {
        out->fileIndex = static_cast<std::uint32_t>(index);
        out->sampleRate = file.toc.sampleRate;
        out->channelCount = file.toc.channelCount;
        out->frameCount = file.toc.frameCount;
        out->contentRevision = file.contentRevision;
        out->sourceGeneration = file.sourceGeneration;
        out->flags = file.toc.flags;
        out->loopStartFrame = file.toc.loopStartFrame;
        out->loopEndFrame = file.toc.loopEndFrame;
    };
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        const StreamFile* file = m_files[i].get();
        if (file != nullptr && file->packPrivate && !file->packRange.expired() &&
            !file->packRange.owner_before(range) && !range.owner_before(file->packRange)) {
            fill(i, *file);
            return VN_OK;
        }
    }
    if (m_lastSourceGeneration == UINT64_MAX) return VN_BACKPRESSURE;
    try {
        auto record = std::make_unique<StreamFile>();
        record->packPrivate = true;
        record->packRange = range;
        record->packBlobOffset = range->blobOffset;
        record->source = range->source;
        record->toc.sampleRate = range->sampleRate;
        record->toc.channelCount = static_cast<std::uint16_t>(range->channelCount);
        record->toc.sampleFormat = kStreamSampleFormatF32Planar;
        record->toc.frameCount = range->frameCount;
        record->toc.chunkCount = static_cast<std::uint32_t>(range->chunkChecksums.size());
        record->toc.flags = range->packFlags & kStreamKnownFlagsMask;
        record->toc.loopStartFrame = range->loop.startFrame;
        record->toc.loopEndFrame = range->loop.endFrame;
        record->chunkFrames = range->chunkFrames;
        record->chunkChecksums = range->chunkChecksums;
        record->contentRevision = range->contentRevision;
        auto empty = std::find_if(m_files.begin(), m_files.end(), [](const auto& file) { return !file; });
        if (empty == m_files.end()) {
            if (m_files.size() == UINT32_MAX) return VN_BACKPRESSURE;
            m_files.emplace_back();
            empty = m_files.end() - 1;
        }
        record->sourceGeneration = ++m_lastSourceGeneration;
        const auto index = static_cast<std::size_t>(empty - m_files.begin());
        fill(index, *record);
        *empty = std::move(record);
        return VN_OK;
    } catch (const std::bad_alloc&) {
        return VN_BACKPRESSURE;
    }
}

bool StreamManager::FactsForFileIndex(std::uint32_t fileIndex,
                                      StreamFacts* out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fileIndex >= m_files.size() || m_files[fileIndex] == nullptr) {
        return false;
    }
    const StreamFile* file = m_files[fileIndex].get();
    out->fileIndex = fileIndex;
    out->sampleRate = file->toc.sampleRate;
    out->channelCount = file->toc.channelCount;
    out->frameCount = file->toc.frameCount;
    out->contentRevision = file->contentRevision;
    out->sourceGeneration = file->sourceGeneration;
    out->flags = file->toc.flags;
    out->loopStartFrame = file->toc.loopStartFrame;
    out->loopEndFrame = file->toc.loopEndFrame;
    return true;
}

std::uint32_t StreamManager::RetiringFileCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uint32_t count = 0;
    for (const std::unique_ptr<StreamFile>& file : m_files) {
        if (file != nullptr && file->retiring) {
            ++count;
        }
    }
    return count;
}

bool StreamManager::HasRetiringFor(VnSoundId soundId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::unique_ptr<StreamFile>& file : m_files) {
        if (file != nullptr && file->retiring &&
            file->retiredFromId == soundId) {
            return true;
        }
    }
    return false;
}

bool StreamManager::AcquirePendingRef(std::uint32_t fileIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fileIndex >= m_files.size() || m_files[fileIndex] == nullptr) {
        return false;
    }
    const auto channels = m_files[fileIndex]->toc.channelCount;
    if (m_pendingRingSlots == UINT32_MAX) return false;
    if (!PrepareRingStorage(OccupiedSlotPrefix(), channels, true)) return false;
    ++m_pendingRingSlots;
    m_files[fileIndex]->pendingRefs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void StreamManager::ReleasePendingRef(std::uint32_t fileIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fileIndex >= m_files.size() || m_files[fileIndex] == nullptr) {
        return;
    }
    assert(m_files[fileIndex]->pendingRefs.load(std::memory_order_relaxed) != 0);
    assert(m_pendingRingSlots != 0);
    --m_pendingRingSlots;
    m_files[fileIndex]->pendingRefs.fetch_sub(1, std::memory_order_relaxed);
}

bool StreamManager::PeekFreeSlot(std::uint32_t* outSlot) const {
    for (std::uint32_t s = 0; s < m_slots.size(); ++s) {
        if (m_slots[s].ring.state.load(std::memory_order_acquire) ==
            StreamSlotState::Free) {
            *outSlot = s;
            return true;
        }
    }
    return false;
}

std::uint32_t StreamManager::FreeSlotCount() const {
    std::uint32_t count = 0;
    for (const StreamSlot& slot : m_slots) {
        if (slot.ring.state.load(std::memory_order_acquire) ==
            StreamSlotState::Free) {
            ++count;
        }
    }
    return count;
}

StreamRing* StreamManager::RingOf(std::uint32_t slot) {
    return &m_slots[slot].ring;
}

bool StreamManager::PrepareSlotWorkingSet(
    std::uint32_t slot, std::uint32_t channels) noexcept {
    return slot < m_slots.size() &&
        PrepareRingStorage(std::max(slot + 1u, OccupiedSlotPrefix()), channels);
}

bool StreamManager::PeekPreparedFreeSlot(
    std::uint32_t channels, std::uint32_t* outSlot) {
    if (!PeekFreeSlot(outSlot)) return false;
    if (RingStorageReady(*outSlot, channels)) return true;
    // A failed private installation may still occupy a retired ring. Finish
    // that detach before reusing the pending reference's protected prefix.
    Request request;
    request.kind = Request::Kind::Reap;
    if (PostAndWait(request) != VN_OK) {
        ReclaimAfterWorkerFailure();
        return false;
    }
    return PeekFreeSlot(outSlot) && RingStorageReady(*outSlot, channels);
}

VnResult StreamManager::ActivateSlot(std::uint32_t slot,
                                     std::uint32_t fileIndex,
                                     std::uint64_t startFrame,
                                     std::uint64_t loopStartFrame,
                                     std::uint64_t loopEndFrame,
                                     std::uint32_t loopCount,
                                     std::uint64_t* outRingToken) {
    if (outRingToken != nullptr) *outRingToken = 0;
    Request request;
    request.kind = Request::Kind::Serve;
    request.onlySlot = slot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (slot >= m_slots.size() || fileIndex >= m_files.size() ||
            m_files[fileIndex] == nullptr ||
            !RingStorageReady(slot, m_files[fileIndex]->toc.channelCount) ||
            ((loopCount != 1u) && loopStartFrame >= loopEndFrame) ||
            m_slots[slot].ring.state.load(std::memory_order_acquire) !=
                StreamSlotState::Free) {
            return VN_DEVICE_ERROR;
        }
        StreamFile& file = *m_files[fileIndex];
        file.activeStreams.fetch_add(1, std::memory_order_relaxed);
        const auto token = InitializeSlot(slot, fileIndex, file, startFrame,
                                         loopStartFrame, loopEndFrame, loopCount);
        request.onlyGeneration = m_slots[slot].ring.generation.load(std::memory_order_relaxed);
        if (outRingToken != nullptr) *outRingToken = token;
    }
    const auto result = PostAndWait(request);
    m_slots[slot].ring.startPromoted.store(true, std::memory_order_release);
    return result;
}

std::uint64_t StreamManager::InitializeSlot(
    std::uint32_t slot, std::uint32_t fileIndex, StreamFile& file,
    std::uint64_t startFrame, std::uint64_t loopStartFrame,
    std::uint64_t loopEndFrame, std::uint32_t loopCount) {
    StreamSlot& s = m_slots[slot];
    PublishPlaybackObservation(s);
    // The slot is Free: neither real-time thread observes it, so these
    // writes are private until Active publishes them. The source reference
    // was acquired under m_mutex, so retirement cannot close it in between.
    s.window.validBytes = 0;
    if (s.promotionPending) {
        assert(m_pendingPromotionCount != 0);
        --m_pendingPromotionCount;
        s.promotionPending = false;
    }
    s.passGoal = 0;
    s.preparationOwner = StreamSlot::PreparationOwner::None;
    s.preparationGeneration = 0;
    s.fileIndex = fileIndex;
    // Seed the source read position. 0 is start-at-file (the original
    // behavior); a nonzero startFrame begins decoding there — the first prebuffer
    // fill reads from it, and the serve loop's chunk-aligned seek + intra-chunk
    // skip honor any frame, so a start inside the loop region resumes as if
    // playback had advanced there. Set before the Active release-store, so the
    // streaming thread's first serve pass observes it. A one-shot test override
    // (consumed here) substitutes its frame when present.
    std::uint64_t effectiveStart = startFrame;
    if (m_testActivationStartFrameSet.exchange(false,
                                               std::memory_order_relaxed)) {
        effectiveStart =
            m_testActivationStartFrame.load(std::memory_order_relaxed);
    }
    s.nextFrame = effectiveStart;
    s.loopStartFrame = loopStartFrame;
    s.loopEndFrame = loopEndFrame;
    s.loopInfinite = (loopCount == 0);
    s.loopPassesRemaining = loopCount;
    assert((loopCount == 1) || loopStartFrame < loopEndFrame);
    s.stagedChunk = kNoChunk;
    s.feedStopped = false;
    s.ring.ResetForUse(file.toc.channelCount);
    s.ring.sourceGeneration = file.sourceGeneration;
    s.ring.contentRevision = file.contentRevision;
    s.prebufferTargetFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(s.ring.FrameCapacity(),
        std::max<std::uint64_t>(1, (static_cast<std::uint64_t>(m_prebufferMs) * file.toc.sampleRate + 999) / 1000)));
    s.observation = {};
    s.observation.structSize = sizeof(s.observation);
    s.observation.factKind = VN_MONITOR_FACT_STREAM_OBSERVATION;
    s.observation.sourceGeneration = file.sourceGeneration;
    s.observation.contentRevision = file.contentRevision;
    s.observation.ringToken = RingToken(slot, s.ring.generation.load(std::memory_order_relaxed));
    s.observation.sourceFrameBegin = s.observation.sourceFrameEnd = effectiveStart;
    s.observation.appliedFrame = UINT64_MAX;
    s.observation.flags = VN_MONITOR_STREAM_FACT_SUPPLY;
    const std::uint32_t ringGeneration =
        s.ring.generation.load(std::memory_order_relaxed);
    const std::uint32_t nowActive =
        m_activeSlots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nowActive > m_streamHighWater) {
        m_streamHighWater = nowActive;
    }
    s.ring.state.store(StreamSlotState::Active, std::memory_order_release);
    return RingToken(slot, ringGeneration);
}

VnResult StreamManager::BeginReplacement(
    const ReplacementRequest& request, PreparedToken* out) {
    if (out == nullptr) return VN_INVALID_ARG;
    *out = {};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (WorkerState() != StreamWorkerState::Ready || m_shutdown)
            return VN_DEVICE_ERROR;
        if (request.fileIndex >= m_files.size() ||
            m_files[request.fileIndex] == nullptr) return VN_STALE_HANDLE;
        StreamFile& file = *m_files[request.fileIndex];
        if (request.sourceGeneration == 0 ||
            file.sourceGeneration != request.sourceGeneration ||
            file.contentRevision != request.contentRevision) return VN_STALE_HANDLE;
        if (request.targetFrame > file.toc.frameCount ||
            request.loopStartFrame > request.loopEndFrame ||
            request.loopEndFrame > file.toc.frameCount ||
            (request.loopCount != 1 &&
             (request.loopStartFrame == request.loopEndFrame ||
              request.targetFrame >= request.loopEndFrame))) return VN_INVALID_ARG;
        std::uint32_t slot = 0;
        if (m_lastPreparationGeneration == UINT64_MAX ||
            FreeSlotCount() <= request.pendingSlotReservations ||
            !PeekFreeSlot(&slot)) return VN_BACKPRESSURE;

        // Pin before private storage preparation. Both failure and publication
        // happen under the same lock that protects retiring file destruction.
        file.activeStreams.fetch_add(1, std::memory_order_relaxed);
        if (!PrepareSlotWorkingSet(slot, file.toc.channelCount)) {
            file.activeStreams.fetch_sub(1, std::memory_order_relaxed);
            return VN_BACKPRESSURE;
        }
        const auto ringToken = InitializeSlot(
            slot, request.fileIndex, file, request.targetFrame,
            request.loopStartFrame, request.loopEndFrame, request.loopCount);
        auto& prepared = m_slots[slot];
        prepared.preparationGeneration = ++m_lastPreparationGeneration;
        prepared.preparationOwner = StreamSlot::PreparationOwner::Prepared;
        prepared.preparedRingToken = ringToken;
        prepared.prebufferTargetFrames = std::min(
            std::max(1u, request.prebufferTargetFrames), prepared.ring.FrameCapacity());
        *out = {slot, prepared.preparationGeneration};
        m_serviceRequested = true;
    }
    m_wake.notify_all();
    return VN_OK;
}

StreamManager::PreparationState StreamManager::QueryReplacement(
    PreparedToken token) const noexcept {
    if (token.slot >= m_slots.size() || token.generation == 0)
        return PreparationState::Invalid;
    const auto& slot = m_slots[token.slot];
    if (slot.preparationGeneration != token.generation)
        return PreparationState::Invalid;
    if (slot.preparationOwner == StreamSlot::PreparationOwner::Cancelled)
        return PreparationState::Cancelled;
    if (slot.preparationOwner != StreamSlot::PreparationOwner::Prepared ||
        slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
        slot.ring.generation.load(std::memory_order_relaxed) !=
            static_cast<std::uint32_t>(slot.preparedRingToken >> 32u))
        return PreparationState::Invalid;
    if (WorkerState() != StreamWorkerState::Ready) return PreparationState::Failed;
    const auto terminal = slot.ring.TerminalReason();
    if (terminal == StreamTerminalReason::IntegrityAbort ||
        terminal == StreamTerminalReason::ProducerFailure) return PreparationState::Failed;
    if (terminal == StreamTerminalReason::NaturalEnd ||
        slot.ring.BufferedFrames() >= slot.prebufferTargetFrames)
        return PreparationState::Ready;
    return PreparationState::Preparing;
}

bool StreamManager::CancelReplacement(PreparedToken token) noexcept {
    const auto state = QueryReplacement(token);
    if (state == PreparationState::Invalid || state == PreparationState::Cancelled)
        return false;
    auto& slot = m_slots[token.slot];
    if (!RetirePreparedSlot(&slot.ring, slot.preparedRingToken)) return false;
    slot.preparationOwner = StreamSlot::PreparationOwner::Cancelled;
    return true;
}

bool StreamManager::CapturePlayback(std::uint64_t token, PlaybackObservation* out) const {
    if (out == nullptr || token == 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto index = static_cast<std::uint32_t>(token);
    if (index >= m_slots.size()) return false;
    const auto& slot = m_slots[index];
    if (slot.ring.generation.load(std::memory_order_acquire) != static_cast<std::uint32_t>(token >> 32) ||
        slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
        slot.fileIndex >= m_files.size() || !m_files[slot.fileIndex]) return false;
    const auto& file = *m_files[slot.fileIndex];
    PlaybackObservation value{};
    value.ringToken = token;
    value.preparationGeneration = slot.preparationGeneration;
    value.sourceGeneration = file.sourceGeneration;
    value.contentRevision = file.contentRevision;
    value.soundId = file.toc.soundId;
    value.sourceFrame = slot.nextFrame;
    value.appliedAudioBlock = slot.ring.appliedAudioBlock.load(std::memory_order_acquire);
    if (value.appliedAudioBlock != 0) {
        value.appliedFrame = slot.ring.appliedFrame.load(std::memory_order_relaxed);
        value.replacementSerial = slot.ring.appliedReplacementSerial.load(std::memory_order_relaxed);
    }
    // With the manager excluded, the write cursor is fixed and the audio
    // consumer can only reduce occupancy. This read is one real sample.
    const auto read = slot.ring.readFrames.load(std::memory_order_acquire);
    const auto written = slot.ring.writeFrames.load(std::memory_order_acquire);
    value.bufferedFrames = static_cast<std::uint32_t>(written - read);
    value.prebufferTargetFrames = slot.prebufferTargetFrames;
    value.terminal = slot.ring.TerminalReason();
    if (value.terminal == StreamTerminalReason::IntegrityAbort ||
        value.terminal == StreamTerminalReason::ProducerFailure || WorkerState() == StreamWorkerState::Failed) {
        value.state = PreparationState::Failed;
        value.result = value.terminal == StreamTerminalReason::IntegrityAbort ? VN_BAD_STREAM_FILE : VN_DEVICE_ERROR;
    } else if (slot.preparationOwner == StreamSlot::PreparationOwner::Cancelled) {
        value.state = PreparationState::Cancelled;
    } else {
        value.state = value.terminal == StreamTerminalReason::NaturalEnd ||
            value.bufferedFrames >= value.prebufferTargetFrames ? PreparationState::Ready : PreparationState::Preparing;
    }
    *out = value;
    return true;
}

VnResult StreamManager::TakeReplacement(
    PreparedToken token, PreparedStream* out) noexcept {
    if (out == nullptr) return VN_INVALID_ARG;
    *out = {};
    const auto state = QueryReplacement(token);
    if (state == PreparationState::Preparing) return VN_BACKPRESSURE;
    if (state == PreparationState::Failed) return VN_DEVICE_ERROR;
    if (state != PreparationState::Ready) return VN_STALE_HANDLE;
    auto& slot = m_slots[token.slot];
    *out = {token.slot, &slot.ring, slot.preparedRingToken};
    slot.preparationOwner = StreamSlot::PreparationOwner::None;
    slot.ring.startPromoted.store(true, std::memory_order_release);
    return VN_OK;
}

VnResult StreamManager::TransferReplacement(
    PreparedToken token, PreparedStream* out) noexcept {
    if (out == nullptr) return VN_INVALID_ARG;
    *out = {};
    const auto state = QueryReplacement(token);
    if (state == PreparationState::Failed) return VN_DEVICE_ERROR;
    if (state != PreparationState::Preparing && state != PreparationState::Ready)
        return VN_STALE_HANDLE;
    auto& slot = m_slots[token.slot];
    *out = {token.slot, &slot.ring, slot.preparedRingToken};
    slot.preparationOwner = StreamSlot::PreparationOwner::None;
    assert(!slot.promotionPending);
    slot.promotionPending = true;
    ++m_pendingPromotionCount;
    return VN_OK;
}

StreamManager::PreparationState StreamManager::PromoteTransferredStream(
    const PreparedStream& stream) noexcept {
    if (stream.slot >= m_slots.size() || stream.ringToken == 0)
        return PreparationState::Invalid;
    auto& slot = m_slots[stream.slot];
    if (stream.ring != &slot.ring || stream.ringToken != slot.preparedRingToken ||
        slot.preparationOwner != StreamSlot::PreparationOwner::None ||
        slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
        slot.ring.generation.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(stream.ringToken >> 32u))
        return PreparationState::Invalid;
    if (WorkerState() != StreamWorkerState::Ready) return PreparationState::Failed;
    const auto terminal = slot.ring.TerminalReason();
    if (terminal == StreamTerminalReason::IntegrityAbort ||
        terminal == StreamTerminalReason::ProducerFailure) return PreparationState::Failed;
    if (terminal != StreamTerminalReason::NaturalEnd &&
        slot.ring.BufferedFrames() < slot.prebufferTargetFrames)
        return PreparationState::Preparing;
    slot.ring.startPromoted.store(true, std::memory_order_release);
    if (slot.promotionPending) {
        assert(m_pendingPromotionCount != 0);
        --m_pendingPromotionCount;
        slot.promotionPending = false;
    }
    return PreparationState::Ready;
}

void StreamManager::ServiceTransferredPreparations() noexcept {
    if (m_pendingPromotionCount == 0) return;
    for (std::uint32_t index = 0; index < m_slots.size(); ++index) {
        auto& slot = m_slots[index];
        if (!slot.promotionPending) continue;
        const auto status = PromoteTransferredStream({index, &slot.ring, slot.preparedRingToken});
        if (status == PreparationState::Invalid || status == PreparationState::Failed) {
            assert(m_pendingPromotionCount != 0);
            --m_pendingPromotionCount;
            slot.promotionPending = false;
        }
    }
}

VnResult StreamManager::PrepareSeekSlot(
    std::uint32_t fileIndex, std::uint64_t contentRevision,
    std::uint64_t targetFrame, std::uint32_t prebufferTargetFrames,
    std::uint32_t* outSlot, StreamRing** outRing,
    std::uint64_t* outRingToken) {
    if (outSlot == nullptr || outRing == nullptr || outRingToken == nullptr) {
        return VN_INVALID_ARG;
    }
    *outSlot = UINT32_MAX;
    *outRing = nullptr;
    *outRingToken = 0;
    StreamFacts facts{};
    if (!FactsForFileIndex(fileIndex, &facts) ||
        facts.contentRevision != contentRevision ||
        targetFrame > facts.frameCount) {
        return VN_STALE_HANDLE;
    }
    std::uint32_t slot = 0;
    if (!PeekFreeSlot(&slot)) return VN_BACKPRESSURE;
    if (!PrepareSlotWorkingSet(slot, facts.channelCount)) return VN_BACKPRESSURE;
    std::uint64_t token = 0;
    const VnResult activated = ActivateSlot(
        slot, fileIndex, targetFrame, 0, facts.frameCount, 1, &token);
    if (activated != VN_OK) {
        StreamRing* ring = RingOf(slot);
        (void)RetirePreparedSlot(ring, token);
        if (m_workerState.load(std::memory_order_acquire) ==
            StreamWorkerState::Failed) {
            ReclaimAfterWorkerFailure();
        }
        return activated;
    }

    StreamRing* ring = RingOf(slot);
    const std::uint32_t required =
        std::min(prebufferTargetFrames, ring->FrameCapacity());
    const StreamTerminalReason terminal = ring->TerminalReason();
    if ((terminal == StreamTerminalReason::IntegrityAbort ||
         terminal == StreamTerminalReason::ProducerFailure) ||
        (ring->BufferedFrames() < required &&
         terminal == StreamTerminalReason::None)) {
        ring->state.store(StreamSlotState::Retired,
                          std::memory_order_release);
        return terminal == StreamTerminalReason::None ? VN_BACKPRESSURE
                                                       : VN_DEVICE_ERROR;
    }
    *outSlot = slot;
    *outRing = ring;
    *outRingToken = token;
    return VN_OK;
}

bool StreamManager::RetirePreparedSlot(
    StreamRing* ring, std::uint64_t ringToken) noexcept {
    if (ring == nullptr || ringToken == 0 ||
        ring->generation.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(ringToken >> 32u) ||
        ring->state.load(std::memory_order_acquire) !=
            StreamSlotState::Active) {
        return false;
    }
    ring->state.store(StreamSlotState::Retired, std::memory_order_release);
    return true;
}

void StreamManager::SyncServe() {
    Request request;
    request.kind = Request::Kind::Serve;
    (void)PostAndWait(request);
}

VnResult StreamManager::PostAndWait(Request& request) {
    std::unique_lock<std::mutex> lock(m_mutex);
    // Refuse up front once the streaming thread has stopped: queueing behind a
    // thread that will never run again is a caller blocked forever, and the
    // subsystem being dead is a system failure rather than a bad file.
    if (m_workerState.load(std::memory_order_acquire) !=
        StreamWorkerState::Ready) {
        request.result = VN_DEVICE_ERROR;
        return request.result;
    }
    try {
        if (StreamRequestFaultForTests() == StreamRequestFault::Enqueue) {
            throw std::bad_alloc{};
        }
        m_requests.push_back(&request);
    } catch (...) {
        request.result = VN_BAD_STREAM_FILE;
        return request.result;
    }
    m_wake.notify_all();
    // Waiting on `done` alone strands the caller if the streaming thread dies
    // before posting it, so the wait also watches the lifecycle state. The
    // bounded re-check is deliberate: a terminal streaming thread may be unable
    // to take the mutex to signal, and a notify issued while this thread is
    // between predicate check and block can be missed. This does not time the
    // request out -- a healthy thread is waited on indefinitely -- it only
    // guarantees the loop cannot outlive the thread it is waiting for.
    while (!request.done) {
        if (m_workerState.load(std::memory_order_acquire) ==
            StreamWorkerState::Failed) {
            // Drop the entry before unwinding: `request` lives on the caller's
            // stack, so leaving a pointer to it queued would hand a dangling
            // pointer to whoever drains the queue next.
            for (auto it = m_requests.begin(); it != m_requests.end(); ++it) {
                if (*it == &request) {
                    m_requests.erase(it);
                    break;
                }
            }
            request.result = VN_DEVICE_ERROR;
            return request.result;
        }
        m_done.wait_for(lock, kServiceInterval);
    }
    return request.result;
}

void StreamManager::ThreadMain() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"HarkenStreaming");
#endif
    try {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (StreamRequestFaultForTests() == StreamRequestFault::WorkerStartup) throw std::bad_alloc{};
        PublishWorkerState(StreamWorkerState::Ready);
        for (;;) {
            if (StreamRequestFaultForTests() == StreamRequestFault::WorkerLoop) throw std::bad_alloc{};
            m_wake.wait_for(lock, kServiceInterval, [&] {
                return m_shutdown || m_serviceRequested || !m_requests.empty();
            });
            if (m_shutdown) {
                PublishWorkerState(StreamWorkerState::Stopping);
                FailQueuedRequestsLocked();
                for (auto* request : m_batch) { request->result = VN_DEVICE_ERROR; request->done = true; }
                m_batch.clear();
                m_done.notify_all();
                break;
            }
            m_serviceRequested = false;
            if (StreamRequestFaultForTests() == StreamRequestFault::WorkerStall) {
                lock.unlock();
                g_streamWorkerStallEntries.fetch_add(1, std::memory_order_acq_rel);
                for (int spin = 0; spin < 2000 &&
                     !g_streamWorkerStallRelease.load(std::memory_order_acquire); ++spin)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                lock.lock();
                if (m_shutdown) continue;
            }
            try {
                SettleIo();
                DetachRetiredSlots();
                CloseDrainedRetiringFiles();
                while (!m_requests.empty()) {
                    m_batch.push_back(m_requests.front());
                    m_requests.pop_front();
                }
                ServiceRequests();
                ServeSlots();
                ServiceRequests();
                DispatchIo();
            } catch (...) {
                for (auto* request : m_batch) { request->result = VN_BAD_STREAM_FILE; request->done = true; }
                m_batch.clear();
                m_done.notify_all();
            }
        }
        PublishWorkerState(StreamWorkerState::Stopped);
    } catch (...) { HandleTerminalWorkerFailure(); }
}

std::uint32_t StreamManager::OccupiedSlotPrefix() const noexcept {
    auto count = static_cast<std::uint32_t>(m_slots.size());
    while (count != 0 && m_slots[count - 1u].ring.state.load(
               std::memory_order_acquire) == StreamSlotState::Free) --count;
    return count;
}

bool StreamManager::PrepareRingStorage(
    std::uint32_t slotPrefix, std::uint32_t channels, bool pending) noexcept {
    if (memguard::IsAudioTagged() || slotPrefix > m_slots.size() ||
        channels == 0 || channels > kMaxChannels) return false;
    if (m_ringBytes < channels * sizeof(float)) return false;
    const auto required = std::min<std::uint64_t>(m_slots.size(),
        static_cast<std::uint64_t>(slotPrefix) + m_pendingRingSlots + (pending ? 1u : 0u));
    return m_ringStorage.ConstructThrough(static_cast<std::uint32_t>(required * m_ringBytes));
}

bool StreamManager::RingStorageReady(std::uint32_t slot, std::uint32_t channels) const noexcept {
    if (slot >= m_slots.size() || channels == 0 || channels > kMaxChannels ||
        m_ringBytes < channels * sizeof(float)) return false;
    return m_ringStorage.ConstructedCount() >= (static_cast<std::uint64_t>(slot) + 1u) * m_ringBytes;
}

void StreamManager::ValidateSourceSlice(SourceRequest& request) {
    auto& record = *request.candidate;
    const StreamReadCancellation cancel{&request.cancelled};
    if (cancel.IsCancelled()) return;
    if (request.phase == 0) {
        if (!record.source) record.source = OpenStreamByteSource(request.path.c_str());
        if (!record.source) { request.result = VN_BAD_STREAM_FILE; return; }
        std::array<std::uint8_t, kStreamHeaderBytes> bytes{};
        if (ReadStreamBytes(*record.source, 0, bytes.data(), bytes.size(), cancel) != StreamReadStatus::Ok ||
            !ParseStreamHeader(bytes.data(), &request.header)) {
            request.result = VN_BAD_STREAM_FILE; return;
        }
        const auto& header = request.header;
        const auto length = record.source->ByteCount();
        if (header.formatVersion != kStreamFormatVersion || header.headerBytes != kStreamHeaderBytes ||
            header.fileBytes != length || header.entryCount != kStreamEntryCount ||
            header.chunkFrames == 0 || header.chunkFrames > kMaxStreamChunkFrames ||
            header.tocOffset < header.headerBytes || header.tocOffset > length ||
            kStreamTocEntryBytes > length - header.tocOffset) {
            request.result = VN_BAD_STREAM_FILE; return;
        }
        request.phase = 1;
        return;
    }
    if (request.phase == 1) {
        std::array<std::uint8_t, kStreamTocEntryBytes> bytes{};
        if (ReadStreamBytes(*record.source, request.header.tocOffset, bytes.data(), bytes.size(), cancel)
            != StreamReadStatus::Ok) { request.result = VN_BAD_STREAM_FILE; return; }
        record.toc = ParseStreamTocEntry(bytes.data());
        const auto& entry = record.toc;
        const std::uint64_t checksumBytes = static_cast<std::uint64_t>(entry.chunkCount) * sizeof(std::uint64_t);
        if (!ValidateStreamEntry(request.header, entry, record.source->ByteCount()) ||
            entry.soundId != request.soundId || checksumBytes > SIZE_MAX) {
            request.result = VN_BAD_STREAM_FILE; return;
        }
        record.chunkFrames = request.header.chunkFrames;
        record.chunkChecksums.resize(entry.chunkCount);
        request.metadataBytes.store(sizeof(StreamFile) +
            static_cast<std::uint64_t>(record.chunkChecksums.capacity()) * sizeof(std::uint64_t),
            std::memory_order_release);
        request.checksum = Fnv1a64(bytes.data(), bytes.size());
        request.semantic = {entry.sampleRate, entry.channelCount, entry.frameCount, entry.sampleFormat,
            request.header.formatVersion, record.chunkFrames, entry.chunkCount,
            (entry.flags & kStreamFlagLoopable) != 0 ? entry.loopStartFrame : 0,
            (entry.flags & kStreamFlagLoopable) != 0
                ? (entry.loopEndFrame == 0 ? entry.frameCount : entry.loopEndFrame) : 0};
        request.revision = BeginMediaStreamContentRevisionV1(request.semantic);
        request.phase = 2;
        return;
    }
    // Only scratch is bounded by the work quantum; the final table is charged
    // to the actual source metadata and may contain any validated chunk count.
    std::array<std::uint8_t, 64 * 1024> bytes{};
    const auto count = std::min<std::uint32_t>(record.toc.chunkCount - request.checksumsRead,
        static_cast<std::uint32_t>(bytes.size() / kStreamChunkChecksumBytes));
    const auto size = static_cast<std::size_t>(count) * kStreamChunkChecksumBytes;
    const auto offset = record.toc.chunkChecksumOffset +
        static_cast<std::uint64_t>(request.checksumsRead) * kStreamChunkChecksumBytes;
    if (ReadStreamBytes(*record.source, offset, bytes.data(), size, cancel) != StreamReadStatus::Ok) {
        request.result = VN_BAD_STREAM_FILE; return;
    }
    request.checksum = Fnv1a64(bytes.data(), size, request.checksum);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto checksum = LoadLE64(bytes.data() + i * kStreamChunkChecksumBytes);
        record.chunkChecksums[request.checksumsRead + i] = checksum;
        request.revision->AddU64(checksum);
    }
    request.checksumsRead += count;
    if (request.checksumsRead == record.toc.chunkCount) {
        if (request.checksum != request.header.tocChecksum) { request.result = VN_BAD_STREAM_FILE; return; }
        record.contentRevision = FinishMediaStreamContentRevisionV1(*request.revision, request.semantic);
        request.phase = 3;
    }
}

void StreamManager::CloseDrainedRetiringFiles() {
    for (std::unique_ptr<StreamFile>& file : m_files) {
        if (file != nullptr && (file->retiring || (file->packPrivate && file->packRange.expired())) &&
            file->activeStreams.load(std::memory_order_relaxed) == 0 &&
            file->pendingRefs.load(std::memory_order_relaxed) == 0) {
            QueueCleanup(std::move(file));
        }
    }
}

void StreamManager::HandleUnregister(Request& request) {
    for (std::unique_ptr<StreamFile>& file : m_files) {
        if (file == nullptr || file->retiring || file->packPrivate ||
            file->soundId != request.soundId) {
            continue;
        }
        if (file->activeStreams.load(std::memory_order_relaxed) != 0 ||
            file->pendingRefs.load(std::memory_order_relaxed) != 0) {
            request.result = VN_IN_USE;
            return;
        }
        QueueCleanup(std::move(file));
        request.result = VN_OK;
        return;
    }
    request.result = VN_UNKNOWN_ID;
}

void StreamManager::DetachRetiredSlots() {
    for (StreamSlot& slot : m_slots) {
        if (slot.ring.state.load(std::memory_order_acquire) !=
            StreamSlotState::Retired) {
            continue;
        }
        if (slot.ioLane >= 0) {
            if (slot.ioLane == kPreparationIoLane)
                m_ioLanes[kPreparationIoLane]->cancelled.store(true, std::memory_order_release);
            else slot.io.cancelled.store(true, std::memory_order_release);
            continue;
        }
        m_files[slot.fileIndex]->activeStreams.fetch_sub(
            1, std::memory_order_relaxed);
        slot.window.validBytes = 0;
        std::vector<std::uint8_t>().swap(slot.window.bytes);
        slot.window.capacityBytes.store(0, std::memory_order_release);
        slot.stagedChunk = kNoChunk;
        slot.feedStopped = false;
        slot.nextFrame = 0;
        slot.loopStartFrame = 0;
        slot.loopEndFrame = 0;
        slot.loopInfinite = false;
        slot.loopPassesRemaining = 1;
        m_activeSlots.fetch_sub(1, std::memory_order_relaxed);
        slot.ring.state.store(StreamSlotState::Free,
                              std::memory_order_release);
    }
}

namespace {
// A stack adapter measures the actual provider boundary, including planar and
// short-read retries. It owns no source and changes neither bytes nor outcomes.
class ObservedStreamRead final : public StreamByteSource {
public:
    ObservedStreamRead(const StreamByteSource& source, VnMonitorStreamFactV1& facts)
        : source_(source), facts_(facts) {}
    std::uint64_t ByteCount() const noexcept override { return source_.ByteCount(); }
    StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t bytes,
        StreamReadCancellation cancellation) const override {
        const auto begin = LogTimestampNs();
        if (facts_.readCalls == 0) facts_.readBeginNs = begin;
        ++facts_.readCalls;
        facts_.readOffset = offset;
        facts_.readRequestedBytes = bytes;
        facts_.lastReadBytes = 0;
        try {
            const auto result = source_.ReadAt(offset, destination, bytes, cancellation);
            facts_.readEndNs = LogTimestampNs();
            facts_.readCostNs += facts_.readEndNs - begin;
            facts_.lastReadBytes = result.bytes;
            facts_.readBytes += result.bytes;
            facts_.readStatus = result.status == StreamReadStatus::Cancelled ? 5u :
                result.status != StreamReadStatus::Ok || result.bytes == 0 || result.bytes > bytes ? 2u : 1u;
            return result;
        } catch (...) {
            facts_.readEndNs = LogTimestampNs();
            facts_.readCostNs += facts_.readEndNs - begin;
            facts_.readStatus = 4;
            throw;
        }
    }
private:
    const StreamByteSource& source_;
    VnMonitorStreamFactV1& facts_;
};
} // namespace

bool StreamManager::RunChunkJob(const ChunkJob& job, std::vector<float>& staging,
    ReadWindow& window, VnMonitorStreamFactV1& facts) {
    facts = {};
    ObservedStreamRead source(*job.file->source, facts);
    if (!ReadChunk(job, staging, window, source)) {
        if (job.cancellation.IsCancelled()) facts.readStatus = 5;
        else if (facts.readStatus == 0 || facts.readStatus == 1) facts.readStatus = 2;
        return false;
    }
    const bool valid = ValidateChunk(job, staging,
        Fnv1a64(staging.data(), static_cast<std::size_t>(job.chunkBytes)));
    if (!valid) facts.readStatus = 3;
    return valid;
}

void StreamManager::CompleteReadObservation(StreamSlot& slot, const ChunkJob& job,
    const VnMonitorStreamFactV1& facts, bool valid, bool exception) noexcept {
    auto& value = slot.observation;
    ++value.readSequence;
    value.sourceFrameBegin = static_cast<std::uint64_t>(job.chunkIndex) * job.chunkFrames;
    value.sourceFrameEnd = std::min(value.sourceFrameBegin + job.chunkFrames, job.file->toc.frameCount);
    value.readOffset = facts.readOffset;
    value.readRequestedBytes = facts.readRequestedBytes;
    value.lastReadBytes = facts.lastReadBytes;
    value.readCalls = facts.readCalls;
    value.readBytes = facts.readBytes;
    value.readCostNs = facts.readCostNs;
    value.readBeginNs = facts.readBeginNs;
    value.readEndNs = facts.readEndNs;
    value.readStatus = exception ? 4u : !valid && facts.readStatus == 0
        ? (job.cancellation.IsCancelled() ? 5u : 3u) : facts.readStatus;
    value.totalReadCalls += facts.readCalls;
    value.totalReadBytes += facts.readBytes;
    value.totalReadCostNs += facts.readCostNs;
}

void StreamManager::PublishPlaybackObservation(StreamSlot& slot) {
    auto& value = slot.observation;
    if (value.ringToken == 0 || !MonitorStreamObservationEnabled()) return;
    const auto state = slot.ring.state.load(std::memory_order_acquire);
    value.preparationGeneration = slot.preparationGeneration;
    value.renderActivation = slot.ring.observationRender.load(std::memory_order_acquire);
    value.ringReadIndex = slot.ring.readFrames.load(std::memory_order_acquire);
    value.ringWriteIndex = slot.ring.writeFrames.load(std::memory_order_acquire);
    value.bufferedFrames = static_cast<std::uint32_t>(value.ringWriteIndex - value.ringReadIndex);
    value.prebufferTargetFrames = slot.prebufferTargetFrames;
    value.terminalReason = static_cast<std::uint32_t>(slot.ring.TerminalReason());
    const bool failed = value.terminalReason == static_cast<std::uint32_t>(StreamTerminalReason::IntegrityAbort) ||
        value.terminalReason == static_cast<std::uint32_t>(StreamTerminalReason::ProducerFailure);
    value.state = failed ? VN_PLAYBACK_READINESS_FAILED : state != StreamSlotState::Active ? VN_PLAYBACK_READINESS_TERMINAL :
        value.terminalReason == static_cast<std::uint32_t>(StreamTerminalReason::NaturalEnd) ||
        value.bufferedFrames >= value.prebufferTargetFrames ? VN_PLAYBACK_READINESS_READY : VN_PLAYBACK_READINESS_PREPARING;
    value.result = !failed ? VN_OK : value.terminalReason == static_cast<std::uint32_t>(StreamTerminalReason::IntegrityAbort)
        ? VN_BAD_STREAM_FILE : VN_DEVICE_ERROR;
    ObserveMonitorStream(value);
    // A Free slot has settled its I/O and returned source ownership. Do not
    // keep replaying a completed generation on every later logic update.
    if (state == StreamSlotState::Free) value.ringToken = 0;
}

void StreamManager::PublishPlaybackObservations() {
    if (!MonitorStreamObservationEnabled()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& slot : m_slots) PublishPlaybackObservation(slot);
}

std::uint64_t StreamManager::ReadWindowBytes() const noexcept {
    std::uint64_t bytes = 0;
    for (const auto& slot : m_slots)
        bytes += slot.window.capacityBytes.load(std::memory_order_acquire);
    return bytes;
}

bool StreamManager::TryCopyReadWindow(const ChunkJob& job, std::vector<float>& staging,
                                      const ReadWindow& window) noexcept {
    if (job.file->packPrivate || job.chunkBytes == 0 || job.chunkBytes / sizeof(float) > staging.size() ||
        window.validBytes > window.bytes.size()) return false;
    const auto offset = StreamChunkFileOffset(job.file->toc.payloadOffset, job.chunkIndex, job.chunkBytes);
    if (offset < window.offset || offset - window.offset > window.validBytes ||
        job.chunkBytes > window.validBytes - (offset - window.offset)) return false;
    std::memcpy(staging.data(), window.bytes.data() + static_cast<std::size_t>(offset - window.offset),
                static_cast<std::size_t>(job.chunkBytes));
    return true;
}

bool StreamManager::ReadChunk(const ChunkJob& job, std::vector<float>& staging,
                              ReadWindow& window, const StreamByteSource& source) {
    staging.resize(static_cast<std::size_t>(job.chunkFrames) * job.channels);
    if (job.file->packPrivate) {
        // Pack blobs are whole-media planar; each channel is a separate range.
        return ReadPlanarMediaChunk(source, job.file->packBlobOffset, job.file->toc.frameCount,
                   job.channels, job.chunkFrames, job.chunkIndex, staging.data(),
                   job.cancellation) == StreamReadStatus::Ok &&
               !job.cancellation.IsCancelled();
    }
    const auto offset = StreamChunkFileOffset(job.file->toc.payloadOffset, job.chunkIndex, job.chunkBytes);
    const auto required = static_cast<std::size_t>(job.chunkBytes);
    const auto direct = [&] {
        return ReadStreamBytes(source, offset, staging.data(), required, job.cancellation) == StreamReadStatus::Ok;
    };
    if (job.cancellation.IsCancelled()) return false;
    if (TryCopyReadWindow(job, staging, window)) return !job.cancellation.IsCancelled();
    window.validBytes = 0;
    const auto length = source.ByteCount();
    if (required >= ReadWindow::kCapacityBytes || offset > length || required > length - offset) return direct();
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(ReadWindow::kCapacityBytes, length - offset));
    if (wanted <= required) return direct();
    try {
        window.bytes.resize(wanted);
        window.capacityBytes.store(window.bytes.capacity(), std::memory_order_release);
    } catch (const std::bad_alloc&) { return direct(); }
    std::size_t received = 0;
    while (received < required) {
        if (job.cancellation.IsCancelled()) return false;
        const auto result = source.ReadAt(offset + received, window.bytes.data() + received,
                                          wanted - received, job.cancellation);
        if (job.cancellation.IsCancelled() || result.status == StreamReadStatus::Cancelled) return false;
        if (result.status != StreamReadStatus::Ok || result.bytes == 0 || result.bytes > wanted - received) {
            // Ahead bytes are optional. Discard the failed attempt and retry
            // exactly the required chunk once, never a canceled operation.
            return wanted > required && direct();
        }
        received += result.bytes;
    }
    window.offset = offset;
    window.validBytes = received;
    std::memcpy(staging.data(), window.bytes.data(), required);
    return !job.cancellation.IsCancelled();
}

bool StreamManager::ValidateChunk(const ChunkJob& job,
                                  const std::vector<float>& staging,
                                  std::uint64_t checksum) {
    const StreamFile& file = *job.file;
    if (checksum != file.chunkChecksums[job.chunkIndex]) {
        return false;
    }
    // A valid checksum does not make NaN/Infinity safe for persistent DSP.
    // Check every real sample before publishing any of this chunk; unused
    // final-chunk padding is checksummed above but is never sent to the ring.
    const std::uint32_t validFrames = StreamChunkValidFrames(
        job.chunkIndex, job.chunkFrames, file.toc.frameCount);
    for (std::uint32_t ch = 0; ch < job.channels; ++ch) {
        const float* samples = staging.data() +
                               static_cast<std::size_t>(ch) * job.chunkFrames;
        for (std::uint32_t frame = 0; frame < validFrames; ++frame) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, samples + frame, sizeof(bits));
            if ((bits & 0x7f800000u) == 0x7f800000u) {
                return false;
            }
        }
    }
    return true;
}

StreamManager::ServiceCursor StreamManager::BeginSlotService(StreamSlot& slot) const {
    const auto limit = m_maxServeFramesForTest.load(std::memory_order_relaxed);
    const auto available = slot.ring.WriteAvailableBytes() / slot.ring.bytesPerFrame;
    return {&slot, limit == 0 ? available : std::min(available, limit), 0, false};
}

void StreamManager::CompleteChunk(StreamSlot& slot, const ChunkJob& job,
                                   bool valid) {
    if (valid) {
        slot.stagedChunk = job.chunkIndex;
    } else {
        // Already published material was verified and drains normally.
        slot.ring.MarkEndOfData(StreamTerminalReason::IntegrityAbort);
        slot.feedStopped = true;
    }
}

bool StreamManager::AdvanceSlot(ServiceCursor& cursor, ChunkJob* job) {
    StreamSlot& slot = *cursor.slot;
    if (slot.feedStopped) {
        return false;
    }
    StreamRing& ring = slot.ring;
    StreamFile& file = *m_files[slot.fileIndex];
    const std::uint32_t chunkFrames = file.chunkFrames;
    const std::uint32_t channels = ring.channelCount;
    const std::uint64_t frameCount = file.toc.frameCount;
    const std::uint64_t chunkBytes =
        StreamChunkBytes(chunkFrames, file.toc.channelCount);

    for (;;) {
        const bool wrapAtLimit =
            slot.loopInfinite || slot.loopPassesRemaining > 1;
        const std::uint64_t limit =
            wrapAtLimit ? slot.loopEndFrame : frameCount;
        if (slot.nextFrame >= limit) {
            if (wrapAtLimit) {
                // The stream thread owns loop traversal. It presents the
                // audio thread with one continuous byte stream by moving
                // the source cursor back to the loop start before any
                // end-of-data signal exists.
                slot.nextFrame = slot.loopStartFrame;
                if (!slot.loopInfinite) {
                    --slot.loopPassesRemaining;
                }
                continue;
            }
            // Every real frame is in the ring; the consumer drains the
            // remainder and the source ends.
            ring.MarkEndOfData(StreamTerminalReason::NaturalEnd);
            slot.feedStopped = true;
            return false;
        }
        if (cursor.framesServed >= cursor.maxServeFrames) return false;
        const StreamSeek seek =
            StreamSeekToFrame(slot.nextFrame, chunkFrames);
        if (slot.stagedChunk != seek.chunkIndex) {
            // Stage and verify the whole chunk before any of it becomes
            // eligible for the ring: frames the audio thread can see have
            // always passed their checksum and finite-sample validation.
            *job = {&file, seek.chunkIndex, chunkFrames, channels, chunkBytes};
            return true;
        }
        const std::uint32_t valid = StreamChunkValidFrames(
            seek.chunkIndex, chunkFrames, frameCount);
        const std::uint32_t remainInChunk = valid - seek.frameInChunk;
        const std::uint32_t remainToLimit = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(limit - slot.nextFrame, remainInChunk));
        const auto span = ring.WriteAvailableBytes() / ring.bytesPerFrame;
        if (span == 0) return false;
        const auto n = std::min({span, cursor.maxServeFrames - cursor.framesServed,
                                 remainInChunk, remainToLimit});
        const auto at = ring.writeIndex.load(std::memory_order_relaxed);
        // Adapt legacy planar source layout by copying sample words verbatim.
        // The ring carries interleaved bytes; sample conversion runs on render.
        for (std::uint32_t frame = 0; frame < n; ++frame)
            for (std::uint32_t ch = 0; ch < channels; ++ch)
                ring.CopyIn(at + (static_cast<std::uint64_t>(frame) * channels + ch) * sizeof(float),
                    slot.staging.data() + static_cast<std::size_t>(ch) * chunkFrames + seek.frameInChunk + frame,
                    sizeof(float));
        ring.PublishBytes(n * ring.bytesPerFrame, n);
        slot.nextFrame += n;
        cursor.framesServed += n;

        const std::uint64_t occupancy =
            ring.writeFrames.load(std::memory_order_relaxed) -
            ring.readFrames.load(std::memory_order_relaxed);
        std::uint64_t seen =
            m_ringHighWaterFrames.load(std::memory_order_relaxed);
        while (occupancy > seen &&
               !m_ringHighWaterFrames.compare_exchange_weak(
                   seen, occupancy, std::memory_order_relaxed)) {
        }
    }
}

} // namespace harken
