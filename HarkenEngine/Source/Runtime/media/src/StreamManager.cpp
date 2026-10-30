#include "harken/media/StreamManager.h"

#include "harken/core/MediaContentRevision.h"
#include "harken/core/Utf8Path.h"

#include <algorithm>
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
    return m_requests.size();
}

StreamManager::StreamManager(const EngineConfig& config)
    : m_ringFrames(static_cast<std::uint32_t>(
          RoundUpPow2(config.streamRingFrames))),
      m_slots(config.maxStreams) {
    const auto channelSamples = static_cast<std::uint64_t>(config.maxStreams) * m_ringFrames;
    if (channelSamples > UINT32_MAX) throw std::bad_alloc();
    for (auto& column : m_ringStorage) {
        if (!column.Reserve(static_cast<std::uint32_t>(channelSamples)))
            throw std::bad_alloc();
    }
    for (std::uint32_t s = 0; s < config.maxStreams; ++s) {
        float* planes[kMaxChannels];
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
            planes[ch] = m_ringStorage[ch].data() +
                static_cast<std::size_t>(s) * m_ringFrames;
        }
        m_slots[s].ring.Wire(planes, m_ringFrames);
    }
    m_thread = std::thread(&StreamManager::ThreadMain, this);

    // Startup handshake. Every stream call blocks until the streaming thread
    // serves it, so a thread that died on the way up would strand its first
    // caller forever. Wait until it has published a verdict; it always
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
    // Close out the rings first: it needs no lock, so voices are ended even if
    // the mutex is the thing that failed.
    EndActiveStreamsOnFailure();
    // Best effort on the queue. A waiter this cannot reach is still released by
    // its own predicate, which re-checks the published state on a bounded
    // interval -- the reason that wait is not a bare notify.
    if (m_mutex.try_lock()) {
        FailQueuedRequestsLocked();
        m_mutex.unlock();
    }
    // Failed last, and deliberately so: publishing it hands slot reclamation to
    // the logic thread, which must not begin while this thread is still
    // touching slots. After this store this thread only returns.
    PublishWorkerState(StreamWorkerState::Failed);
}

void StreamManager::ReclaimAfterWorkerFailure() {
    if (m_workerState.load(std::memory_order_acquire) !=
        StreamWorkerState::Failed) {
        return;
    }
    // Defence in depth for a slot activated after the failure: a play that
    // passed its liveness pre-check just before the state flipped publishes an
    // Active slot nobody will ever feed or end. Marking end-of-data here closes
    // it on the next pass. Idempotent -- the ring's compare-exchange simply
    // fails on a slot already terminated or already committed to a park -- and
    // it runs before the detach below so a slot the audio thread retires as a
    // result is reclaimed without waiting another pass.
    EndActiveStreamsOnFailure();
    // Ownership transfer, not a second owner: slots are normally returned to
    // Free by the streaming thread, so with that thread gone nothing would ever
    // reclaim a slot the audio thread has retired and the stream budget would
    // drain away for the rest of the session. Failed is published only after the
    // streaming thread has stopped touching slots, so this is the sole writer.
    DetachRetiredSlots();
    // Retiring files are likewise closed only by the streaming thread, so
    // without this a replaced-away generation stays open for the session and
    // keeps counting against the retiring total long after its last consumer
    // ended. Takes the mutex for the m_files mutation, uncontended now.
    CloseDrainedRetiringFiles();
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

VnResult StreamManager::RegisterFile(VnSoundId soundId,
                                     const char* utf8Path) {
    Request request;
    request.kind = Request::Kind::Register;
    request.soundId = soundId;
    request.utf8Path = utf8Path;
    return PostAndWait(request);
}

VnResult StreamManager::UnregisterFile(VnSoundId soundId) {
    Request request;
    request.kind = Request::Kind::Unregister;
    request.soundId = soundId;
    return PostAndWait(request);
}

VnResult StreamManager::ReplaceFile(VnSoundId soundId,
                                    const char* utf8Path) {
    Request request;
    request.kind = Request::Kind::Replace;
    request.soundId = soundId;
    request.utf8Path = utf8Path;
    return PostAndWait(request);
}

bool StreamManager::FindFile(VnSoundId soundId, StreamFacts* out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        const StreamFile* file = m_files[i].get();
        if (file == nullptr || file->retiring || file->soundId != soundId) {
            continue;
        }
        out->fileIndex = static_cast<std::uint32_t>(i);
        out->sampleRate = file->toc.sampleRate;
        out->channelCount = file->toc.channelCount;
        out->frameCount = file->toc.frameCount;
        out->contentRevision = file->contentRevision;
        out->flags = file->toc.flags;
        out->loopStartFrame = file->toc.loopStartFrame;
        out->loopEndFrame = file->toc.loopEndFrame;
        return true;
    }
    return false;
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
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        if (m_pendingRingSlots[ch] == UINT32_MAX) return false;
    }
    if (!PrepareRingStorage(OccupiedSlotPrefix(), channels, true)) return false;
    for (std::uint32_t ch = 0; ch < channels; ++ch) ++m_pendingRingSlots[ch];
    m_files[fileIndex]->pendingRefs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void StreamManager::ReleasePendingRef(std::uint32_t fileIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fileIndex >= m_files.size() || m_files[fileIndex] == nullptr) {
        return;
    }
    assert(m_files[fileIndex]->pendingRefs.load(std::memory_order_relaxed) != 0);
    for (std::uint32_t ch = 0; ch < m_files[fileIndex]->toc.channelCount; ++ch) {
        assert(m_pendingRingSlots[ch] != 0);
        --m_pendingRingSlots[ch];
    }
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
    request.kind = Request::Kind::Serve;
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
    if (slot >= m_slots.size() || fileIndex >= m_files.size() ||
        m_files[fileIndex] == nullptr ||
        !RingStorageReady(slot, m_files[fileIndex]->toc.channelCount) ||
        ((loopCount != 1u) && loopStartFrame >= loopEndFrame) ||
        m_slots[slot].ring.state.load(std::memory_order_acquire) !=
            StreamSlotState::Free) {
        return VN_DEVICE_ERROR;
    }
    StreamSlot& s = m_slots[slot];
    // The slot is Free: neither real-time thread observes it, so these
    // plain writes are private until the Active release-store publishes
    // them. The file record itself cannot be mutated concurrently — table
    // changes happen on the streaming thread only while the logic thread
    // is blocked inside a register/unregister request, and this call runs
    // on the logic thread.
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
    s.ring.ResetForUse(m_files[fileIndex]->toc.channelCount);
    const std::uint32_t ringGeneration =
        s.ring.generation.load(std::memory_order_relaxed);
    m_files[fileIndex]->activeStreams.fetch_add(1,
                                                std::memory_order_relaxed);
    const std::uint32_t nowActive =
        m_activeSlots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nowActive > m_streamHighWater) {
        m_streamHighWater = nowActive;
    }
    s.ring.state.store(StreamSlotState::Active, std::memory_order_release);
    if (outRingToken != nullptr) {
        *outRingToken = RingToken(slot, ringGeneration);
    }
    // Kick the streaming thread so prebuffering starts now rather than at
    // the next periodic wakeup.
    Request request;
    request.kind = Request::Kind::Serve;
    const VnResult result = PostAndWait(request);
    return result;
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
        std::min(prebufferTargetFrames, ring->capacity);
    const StreamTerminalReason terminal = ring->TerminalReason();
    if ((terminal == StreamTerminalReason::IntegrityAbort ||
         terminal == StreamTerminalReason::ProducerFailure) ||
        (ring->ReadAvailable() < required &&
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
    // OS-visible thread identity for debuggers and profilers.
    SetThreadDescription(GetCurrentThread(), L"HarkenStreaming");
#endif
    // Function-level last line of defense. Control reaches this outer catch if
    // startup construction of the lock or reusable batch fails, or if the
    // per-iteration recovery relock throws. In either terminal case there is no
    // usable iteration state through which to signal waiters. Swallowing here
    // keeps the literal guarantee that no exception escapes the thread entry
    // (which would std::terminate the process). See
    // .claude/rules/local/audio-dsp-discipline.md ("thread exception boundary").
    try {
        std::unique_lock<std::mutex> lock(m_mutex);
        // The batch is a member, constructed before this thread existed, so
        // each iteration reuses it through non-allocating clear and swap
        // operations and startup allocates nothing.
        std::deque<Request*>& batch = m_batch;
        std::size_t dispatched = 0;
        if (StreamRequestFaultForTests() == StreamRequestFault::WorkerStartup) {
            throw std::bad_alloc{};
        }
        // Ready before the first wait: a caller that observes it knows this
        // thread got far enough to serve.
        PublishWorkerState(StreamWorkerState::Ready);
        for (;;) {
            if (StreamRequestFaultForTests() == StreamRequestFault::WorkerLoop) {
                throw std::bad_alloc{};
            }
            // Iteration-level boundary: every throw source in the loop body —
            // wait_for, unlock, request dispatch, ServeSlots, and the relocking
            // epilogue — is inside this try. An exception must never escape the
            // thread entry and must never leave a waiter unsignalled
            // (PostAndWait blocks on request->done, so a skipped completion is
            // a permanent hang). Each request is also dispatched under its own
            // inner catch so one failing request fails in isolation without
            // touching its batch mates.
            try {
                batch.clear();
                dispatched = 0;
                m_wake.wait_for(lock, kServiceInterval, [&] {
                    return m_shutdown || !m_requests.empty();
                });
                if (m_shutdown) {
                    // Anything queued at this instant would otherwise be
                    // dropped un-completed, because the swap below is what
                    // normally claims it -- and its caller is blocked on a
                    // completion nobody would post.
                    PublishWorkerState(StreamWorkerState::Stopping);
                    FailQueuedRequestsLocked();
                    break;
                }
                batch.swap(m_requests);
                lock.unlock();

                if (StreamRequestFaultForTests() ==
                    StreamRequestFault::WorkerStall) {
                    // Held here with the request mutex released, so a test can
                    // queue another request behind this pass and drive shutdown
                    // deterministically. Bounded so a released-too-late test
                    // fails rather than hanging the suite.
                    g_streamWorkerStallEntries.fetch_add(
                        1, std::memory_order_acq_rel);
                    for (int spin = 0; spin < 2000; ++spin) {
                        if (g_streamWorkerStallRelease.load(
                                std::memory_order_acquire)) {
                            break;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(5));
                    }
                }

                // Detach before requests so an unregister following a voice's
                // end never sees a phantom active stream; then close retiring
                // files the detach just drained, so a replaced-away
                // generation's handle is released as soon as its last consumer
                // ends.
                DetachRetiredSlots();
                CloseDrainedRetiringFiles();
                for (Request* request : batch) {
                    try {
                        switch (request->kind) {
                        case Request::Kind::Register:
                            HandleRegister(*request);
                            break;
                        case Request::Kind::Unregister:
                            HandleUnregister(*request);
                            break;
                        case Request::Kind::Replace:
                            HandleReplace(*request);
                            break;
                        case Request::Kind::Serve:
                            // The pass around this switch IS the service; the
                            // request only sequences the caller against its
                            // completion.
                            break;
                        }
                    } catch (...) {
                        // One request's failure — resource exhaustion for that
                        // request included — is that request's bad file, not
                        // the batch's and not the thread's.
                        request->result = VN_BAD_STREAM_FILE;
                    }
                    ++dispatched;
                }
                if (!m_throttled.load(std::memory_order_relaxed)) {
                    ServeSlots();
                }

                // Normal-path epilogue: relock and complete every request. A
                // Serve keeps its default VN_OK because the service pass above
                // genuinely ran this wake.
                lock.lock();
                for (Request* request : batch) {
                    request->done = true;
                }
                if (!batch.empty()) {
                    m_done.notify_all();
                }
            } catch (...) {
                // Best-effort recovery for a throw anywhere in the iteration
                // body. Thread survival takes priority over this one wake. The
                // lock may or may not be held depending on where the throw
                // landed: wait_for reacquires it on the way out (its
                // postcondition) and a failed unlock leaves it held, while a
                // throw in the unlocked dispatch/ServeSlots/epilogue region
                // leaves it released. Relock only when not already owned —
                // relocking a held non-recursive mutex would deadlock — and
                // guard that relock, since it is itself a throw source.
                //
                // Completion marks the undispatched suffix and every Serve
                // request VN_BAD_STREAM_FILE: a Serve carries no result of its
                // own, so its default VN_OK would falsely report "service
                // completed" when the pass never finished; requests already
                // dispatched to a handler keep the result the handler produced.
                // Then fall through to the next iteration, which re-enters
                // wait_for with the lock held — the invariant it requires.
                try {
                    if (!lock.owns_lock()) {
                        lock.lock();
                    }
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                        if (i >= dispatched ||
                            batch[i]->kind == Request::Kind::Serve) {
                            batch[i]->result = VN_BAD_STREAM_FILE;
                        }
                        batch[i]->done = true;
                    }
                    if (!batch.empty()) {
                        m_done.notify_all();
                    }
                } catch (...) {
                    // The relock itself threw: the mutex is unusable, so there
                    // is no way to signal waiters from here and no point
                    // spinning on a broken lock. Propagate to the
                    // function-level catch, which ends the thread.
                    throw;
                }
            }
        }
        PublishWorkerState(StreamWorkerState::Stopped);
    } catch (...) {
        // Terminal: startup construction failed or recovery could not reacquire
        // the lock. The process stays alive, but this subsystem is finished, so
        // every waiter is released, every fed ring is closed out, and the state
        // is published as Failed for the constructor and for later callers.
        HandleTerminalWorkerFailure();
    }
}

void StreamManager::HandleRegister(Request& request) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A retiring file is not registered under any id — it neither
        // collides with a fresh registration nor blocks one.
        for (const std::unique_ptr<StreamFile>& file : m_files) {
            if (file != nullptr && !file->retiring &&
                file->soundId == request.soundId) {
                request.result = VN_IN_USE;
                return;
            }
        }
    }

    auto record = std::make_unique<StreamFile>();
    record->soundId = request.soundId;
    const VnResult valid = OpenAndValidate(request, *record);
    if (valid != VN_OK) {
        request.result = valid;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::unique_ptr<StreamFile>& file : m_files) {
            if (file == nullptr) {
                file = std::move(record);
                request.result = VN_OK;
                return;
            }
        }
        m_files.push_back(std::move(record));
    }
    request.result = VN_OK;
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
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        if (ch >= channels && m_pendingRingSlots[ch] == 0) continue;
        const auto required = std::min<std::uint64_t>(m_slots.size(),
            static_cast<std::uint64_t>(slotPrefix) + m_pendingRingSlots[ch] +
                (pending && ch < channels ? 1u : 0u));
        if (!m_ringStorage[ch].ConstructThrough(
                static_cast<std::uint32_t>(required * m_ringFrames))) return false;
    }
    return true;
}

bool StreamManager::RingStorageReady(
    std::uint32_t slot, std::uint32_t channels) const noexcept {
    if (slot >= m_slots.size() || channels == 0 || channels > kMaxChannels) return false;
    const auto samples = (static_cast<std::uint64_t>(slot) + 1u) * m_ringFrames;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        if (m_ringStorage[ch].ConstructedCount() < samples) return false;
    }
    return true;
}

VnResult StreamManager::OpenAndValidate(Request& request,
                                        StreamFile& record) {
    // Public path contract is UTF-8; decode through the shared validator so
    // ill-formed bytes refuse the file instead of throwing out of the ABI on
    // the streaming thread, and the ANSI codepage never participates.
    std::filesystem::path path;
    if (!TryMakeUtf8Path(request.utf8Path, &path)) {
        return VN_BAD_STREAM_FILE;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size < kStreamHeaderBytes) {
        return VN_BAD_STREAM_FILE;
    }
    record.file.open(path, std::ios::binary);
    if (!record.file) {
        return VN_BAD_STREAM_FILE;
    }

    std::uint8_t headerBytes[kStreamHeaderBytes];
    record.file.read(reinterpret_cast<char*>(headerBytes),
                     kStreamHeaderBytes);
    if (record.file.gcount() != kStreamHeaderBytes) {
        return VN_BAD_STREAM_FILE;
    }
    StreamHeader header;
    if (!ParseStreamHeader(headerBytes, &header)) {
        return VN_BAD_STREAM_FILE;
    }
    if (header.formatVersion != kStreamFormatVersion ||
        header.headerBytes != kStreamHeaderBytes ||
        header.fileBytes != size ||
        header.entryCount != kStreamEntryCount ||
        header.chunkFrames == 0 ||
        header.chunkFrames > kMaxStreamChunkFrames) {
        return VN_BAD_STREAM_FILE;
    }
    if (header.tocOffset < header.headerBytes ||
        header.tocOffset > size ||
        kStreamTocEntryBytes > size - header.tocOffset) {
        return VN_BAD_STREAM_FILE;
    }

    std::uint8_t entryBytes[kStreamTocEntryBytes];
    record.file.seekg(header.tocOffset);
    record.file.read(reinterpret_cast<char*>(entryBytes),
                     kStreamTocEntryBytes);
    if (record.file.gcount() != kStreamTocEntryBytes) {
        return VN_BAD_STREAM_FILE;
    }
    const StreamTocEntry entry = ParseStreamTocEntry(entryBytes);
    if (!ValidateStreamEntry(header, entry, size)) {
        return VN_BAD_STREAM_FILE;
    }
    // The TOC carries the id the file was compiled for precisely so a
    // mislabeled file is caught here at registration, instead of playing
    // the wrong content at playback.
    if (entry.soundId != request.soundId) {
        return VN_BAD_STREAM_FILE;
    }

    // The chunk-checksum table loads into memory for the file's lifetime
    // and its bytes join the TOC under tocChecksum, so a corrupted table
    // is refused here instead of surfacing later as false mid-stream
    // mismatches.
    std::vector<std::uint8_t> checksumBytes(
        static_cast<std::size_t>(entry.chunkCount) *
        kStreamChunkChecksumBytes);
    record.file.seekg(static_cast<std::streamoff>(
        entry.chunkChecksumOffset));
    record.file.read(reinterpret_cast<char*>(checksumBytes.data()),
                     static_cast<std::streamsize>(checksumBytes.size()));
    if (record.file.gcount() !=
        static_cast<std::streamsize>(checksumBytes.size())) {
        return VN_BAD_STREAM_FILE;
    }
    std::uint64_t checksum = Fnv1a64(entryBytes, kStreamTocEntryBytes);
    checksum = Fnv1a64(checksumBytes.data(), checksumBytes.size(), checksum);
    if (checksum != header.tocChecksum) {
        return VN_BAD_STREAM_FILE;
    }

    record.toc = entry;
    record.chunkFrames = header.chunkFrames;
    record.chunkChecksums.resize(entry.chunkCount);
    for (std::uint32_t i = 0; i < entry.chunkCount; ++i) {
        record.chunkChecksums[i] = LoadLE64(
            checksumBytes.data() +
            static_cast<std::size_t>(i) * kStreamChunkChecksumBytes);
    }
    const MediaStreamSemanticHeaderV1 semanticHeader{
        entry.sampleRate,
        entry.channelCount,
        entry.frameCount,
        entry.sampleFormat,
        header.formatVersion,
        header.chunkFrames,
        entry.chunkCount,
    };
    if (!ComputeMediaStreamContentRevisionV1(
            semanticHeader, record.chunkChecksums.data(),
            &record.contentRevision)) {
        return VN_BAD_STREAM_FILE;
    }
    return VN_OK;
}

void StreamManager::HandleReplace(Request& request) {
    // Reap any generation the last detach pass drained before the
    // structural check, so only a genuinely still-consumed old file
    // refuses.
    CloseDrainedRetiringFiles();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool registered = false;
        for (const std::unique_ptr<StreamFile>& file : m_files) {
            if (file != nullptr && !file->retiring &&
                file->soundId == request.soundId) {
                registered = true;
                break;
            }
        }
        if (!registered) {
            request.result = VN_UNKNOWN_ID; // replace never registers
            return;
        }
        for (const std::unique_ptr<StreamFile>& file : m_files) {
            if (file != nullptr && file->retiring &&
                file->retiredFromId == request.soundId) {
                request.result = VN_IN_USE; // one retiring generation
                return;
            }
        }
    }

    // Validate the new file completely before touching the registration —
    // a refusal leaves the old file bound to the id exactly as it was. No
    // register/unregister can interleave: the logic thread is blocked in
    // this very request.
    auto record = std::make_unique<StreamFile>();
    record->soundId = request.soundId;
    const VnResult valid = OpenAndValidate(request, *record);
    if (valid != VN_OK) {
        request.result = valid;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Secure the new file's seat BEFORE the old registration changes: a
        // table growth that cannot be satisfied must fail with the id still
        // fully bound to the old file, never mid-swap. With a free slot (or
        // the reserve done) the placement below cannot throw.
        bool haveFreeSlot = false;
        for (const std::unique_ptr<StreamFile>& file : m_files) {
            if (file == nullptr) {
                haveFreeSlot = true;
                break;
            }
        }
        if (!haveFreeSlot) {
            m_files.reserve(m_files.size() + 1);
        }
        // The old file becomes a retiring generation: unregistered from the
        // id (new lookups resolve the new file) but open for the slots and
        // pending pins still reading it by index.
        for (std::unique_ptr<StreamFile>& file : m_files) {
            if (file != nullptr && !file->retiring &&
                file->soundId == request.soundId) {
                file->retiring = true;
                file->retiredFromId = request.soundId;
                file->soundId = {};
                break;
            }
        }
        for (std::unique_ptr<StreamFile>& file : m_files) {
            if (file == nullptr) {
                file = std::move(record);
                break;
            }
        }
        if (record != nullptr) {
            m_files.push_back(std::move(record)); // nothrow: reserved above
        }
    }
    request.result = VN_OK;
    // An old generation nobody was consuming closes right away.
    CloseDrainedRetiringFiles();
}

void StreamManager::CloseDrainedRetiringFiles() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::unique_ptr<StreamFile>& file : m_files) {
        if (file != nullptr && file->retiring &&
            file->activeStreams.load(std::memory_order_relaxed) == 0 &&
            file->pendingRefs.load(std::memory_order_relaxed) == 0) {
            file.reset(); // closes the handle; the index becomes reusable
        }
    }
}

void StreamManager::HandleUnregister(Request& request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::unique_ptr<StreamFile>& file : m_files) {
        if (file == nullptr || file->retiring ||
            file->soundId != request.soundId) {
            continue;
        }
        if (file->activeStreams.load(std::memory_order_relaxed) != 0 ||
            file->pendingRefs.load(std::memory_order_relaxed) != 0) {
            request.result = VN_IN_USE;
            return;
        }
        file.reset(); // closes the handle
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
        m_files[slot.fileIndex]->activeStreams.fetch_sub(
            1, std::memory_order_relaxed);
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

void StreamManager::ServeSlots() {
    for (StreamSlot& slot : m_slots) {
        if (slot.ring.state.load(std::memory_order_acquire) ==
            StreamSlotState::Active) {
            ServeOneSlot(slot);
        }
    }
}

bool StreamManager::RunChunkJob(const ChunkJob& job,
                                std::vector<float>& staging) {
    StreamFile& file = *job.file;
    staging.resize(static_cast<std::size_t>(job.chunkFrames) * job.channels);
    file.file.clear();
    file.file.seekg(static_cast<std::streamoff>(
        StreamChunkFileOffset(file.toc.payloadOffset, job.chunkIndex,
                              job.chunkBytes)));
    file.file.read(reinterpret_cast<char*>(staging.data()),
                   static_cast<std::streamsize>(job.chunkBytes));
    if (file.file.gcount() != static_cast<std::streamsize>(job.chunkBytes)) {
        return false;
    }
    if (Fnv1a64(staging.data(), static_cast<std::size_t>(job.chunkBytes)) !=
        file.chunkChecksums[job.chunkIndex]) {
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

void StreamManager::ServeOneSlot(StreamSlot& slot) {
    if (slot.feedStopped) {
        return;
    }
    StreamRing& ring = slot.ring;
    StreamFile& file = *m_files[slot.fileIndex];
    const std::uint32_t chunkFrames = file.chunkFrames;
    const std::uint32_t channels = ring.channelCount;
    const std::uint64_t frameCount = file.toc.frameCount;
    const std::uint64_t chunkBytes =
        StreamChunkBytes(chunkFrames, file.toc.channelCount);
    std::uint32_t framesServed = 0;
    const std::uint32_t testLimit =
        m_maxServeFramesForTest.load(std::memory_order_relaxed);
    const std::uint32_t availableAtEntry = ring.WriteAvailable();
    const std::uint32_t maxServeFrames = testLimit == 0
        ? availableAtEntry : std::min(availableAtEntry, testLimit);

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
            return;
        }
        if (framesServed >= maxServeFrames) return;
        const StreamSeek seek =
            StreamSeekToFrame(slot.nextFrame, chunkFrames);
        if (slot.stagedChunk != seek.chunkIndex) {
            // Stage and verify the whole chunk before any of it becomes
            // eligible for the ring: frames the audio thread can see have
            // always passed their checksum and finite-sample validation.
            const ChunkJob job{&file, seek.chunkIndex, chunkFrames, channels,
                               chunkBytes};
            if (!RunChunkJob(job, slot.staging)) {
                // Mid-stream integrity failure: stop feeding; what is
                // already buffered was verified and drains normally, then
                // the source ends.
                ring.MarkEndOfData(StreamTerminalReason::IntegrityAbort);
                slot.feedStopped = true;
                return;
            }
            slot.stagedChunk = seek.chunkIndex;
        }
        const std::uint32_t valid = StreamChunkValidFrames(
            seek.chunkIndex, chunkFrames, frameCount);
        const std::uint32_t remainInChunk = valid - seek.frameInChunk;
        const std::uint32_t remainToLimit = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(limit - slot.nextFrame, remainInChunk));
        float* dst[kMaxChannels];
        const std::uint32_t span = ring.ContiguousWrite(dst);
        if (span == 0) {
            return; // ring full; the next pass resumes here
        }
        const std::uint32_t serveBudget =
            std::min(span, maxServeFrames - framesServed);
        const std::uint32_t n =
            std::min({serveBudget, remainInChunk, remainToLimit});
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            std::memcpy(dst[ch],
                        slot.staging.data() +
                            static_cast<std::size_t>(ch) * chunkFrames +
                            seek.frameInChunk,
                        static_cast<std::size_t>(n) * sizeof(float));
        }
        ring.PublishWrite(n);
        slot.nextFrame += n;
        framesServed += n;

        const std::uint64_t occupancy =
            ring.writeIndex.load(std::memory_order_relaxed) -
            ring.readIndex.load(std::memory_order_relaxed);
        std::uint64_t seen =
            m_ringHighWaterFrames.load(std::memory_order_relaxed);
        while (occupancy > seen &&
               !m_ringHighWaterFrames.compare_exchange_weak(
                   seen, occupancy, std::memory_order_relaxed)) {
        }
    }
}

} // namespace harken
