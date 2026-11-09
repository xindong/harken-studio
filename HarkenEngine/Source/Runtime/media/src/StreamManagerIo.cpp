#include "harken/media/StreamManager.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

namespace harken {
namespace {
std::atomic<std::uint64_t> g_sourceRequestIdentity{0};
std::uint64_t NextSourceRequestIdentity() noexcept {
    auto previous = g_sourceRequestIdentity.load(std::memory_order_relaxed);
    while (previous != UINT64_MAX) {
        if (g_sourceRequestIdentity.compare_exchange_weak(previous, previous + 1,
                std::memory_order_relaxed)) return previous + 1;
    }
    return 0;
}
constexpr auto kIoPoll = std::chrono::milliseconds(2);
}

VnResult StreamManager::BeginSource(VnSoundId id,
    std::shared_ptr<const StreamByteSource> source, bool replace,
    std::uint64_t expected, SourceToken* out) {
    if (!source) return VN_INVALID_ARG;
    return BeginSourceImpl(id, std::move(source), nullptr, replace, expected, out);
}

VnResult StreamManager::BeginFileSource(VnSoundId id, const char* path,
    bool replace, std::uint64_t expected, SourceToken* out) {
    if (path == nullptr) return VN_INVALID_ARG;
    return BeginSourceImpl(id, {}, path, replace, expected, out);
}

VnResult StreamManager::BeginSourceImpl(VnSoundId id,
    std::shared_ptr<const StreamByteSource> source, const char* path,
    bool replace, std::uint64_t expected, SourceToken* out) {
    if (out == nullptr) return VN_INVALID_ARG;
    *out = {};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || WorkerState() != StreamWorkerState::Ready) return VN_DEVICE_ERROR;
    for (const auto& pending : m_sourceRequests)
        if (pending && !pending->discard && pending->soundId == id) return VN_IN_USE;
    StreamFile* current = nullptr;
    for (const auto& file : m_files) {
        if (!file || file->packPrivate) continue;
        if (!file->retiring && file->soundId == id) current = file.get();
        if (replace && file->retiring && file->retiredFromId == id) return VN_IN_USE;
    }
    if (!replace && current) return VN_IN_USE;
    if (replace && !current) return VN_UNKNOWN_ID;
    if (replace && expected != current->sourceGeneration) return VN_STALE_HANDLE;
    try {
        if (StreamRequestFaultForTests() == StreamRequestFault::Enqueue) return VN_BAD_STREAM_FILE;
        auto pending = std::make_unique<SourceRequest>();
        pending->candidate = std::make_unique<StreamFile>();
        pending->soundId = id;
        pending->candidate->soundId = id;
        pending->replace = replace;
        pending->expectedOldGeneration = expected;
        if (path) pending->path = path;
        auto position = std::find_if(m_sourceRequests.begin(), m_sourceRequests.end(),
                                    [](const auto& item) { return !item; });
        if (position == m_sourceRequests.end()) {
            m_sourceRequests.emplace_back();
            position = m_sourceRequests.end() - 1;
        }
        pending->generation = NextSourceRequestIdentity();
        if (pending->generation == 0) return VN_BACKPRESSURE;
        // No potentially failing operation follows this ownership transfer.
        pending->candidate->source = std::move(source);
        out->value = pending->generation;
        *position = std::move(pending);
        m_serviceRequested = true;
        m_wake.notify_all();
        return VN_OK;
    } catch (const std::bad_alloc&) { return VN_BACKPRESSURE; }
}

StreamManager::PreparationState StreamManager::QuerySource(SourceToken token, VnResult* error,
    VnSoundId* soundId, bool* replace) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (error) *error = VN_STALE_HANDLE;
    for (const auto& request : m_sourceRequests) {
        if (!request || request->generation != token.value) continue;
        if (soundId) *soundId = request->soundId;
        if (replace) *replace = request->replace;
        if (error) *error = WorkerState() == StreamWorkerState::Failed ? VN_DEVICE_ERROR
            : (request->ioLane < 0 ? request->result : VN_OK);
        return request->state;
    }
    return PreparationState::Invalid;
}

bool StreamManager::HasPendingSource(VnSoundId id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_sourceRequests.begin(), m_sourceRequests.end(), [&](const auto& request) {
        return request && !request->discard && request->soundId == id;
    });
}

std::uint64_t StreamManager::SourcePreparationBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uint64_t bytes = m_sourceRequests.capacity() * sizeof(std::unique_ptr<SourceRequest>);
    for (const auto& request : m_sourceRequests) {
        if (!request) continue;
        bytes += sizeof(SourceRequest) + request->path.capacity();
        bytes += request->metadataBytes.load(std::memory_order_acquire);
    }
    return bytes;
}

bool StreamManager::CancelSource(SourceToken token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& request : m_sourceRequests) {
        if (!request || request->generation != token.value || request->discard) continue;
        request->discard = true;
        request->state = PreparationState::Cancelled;
        request->cancelled.store(true, std::memory_order_release);
        m_serviceRequested = true;
        m_wake.notify_all();
        return true;
    }
    return false;
}

VnResult StreamManager::TakeSource(SourceToken token, StreamFacts* out, VnResult authority) {
    if (out) *out = {};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (WorkerState() != StreamWorkerState::Ready || m_shutdown) return VN_DEVICE_ERROR;
    for (auto& request : m_sourceRequests) {
        if (!request || request->generation != token.value) continue;
        if (request->state == PreparationState::Preparing) return VN_BACKPRESSURE;
        if (request->state == PreparationState::Failed) return request->result;
        if (request->state != PreparationState::Ready) return VN_STALE_HANDLE;
        VnResult refusal = VN_OK;
        bool retryable = false;
        StreamFile* previous = nullptr;
        for (auto& file : m_files) {
            if (!file || file->packPrivate) continue;
            if (!file->retiring && file->soundId == request->soundId) previous = file.get();
            if (request->replace && file->retiring && file->retiredFromId == request->soundId) {
                refusal = VN_IN_USE;
                retryable = true;
            }
        }
        if (WorkerState() != StreamWorkerState::Ready || m_shutdown) refusal = VN_DEVICE_ERROR;
        else if (request->replace && (!previous || previous->sourceGeneration != request->expectedOldGeneration))
            refusal = VN_STALE_HANDLE;
        else if (!request->replace && previous) refusal = VN_IN_USE;
        else if (m_lastSourceGeneration == UINT64_MAX) refusal = VN_BACKPRESSURE;
        if (authority != VN_OK) { refusal = authority; retryable = false; }
        if (refusal == VN_OK) {
            try {
                auto empty = std::find_if(m_files.begin(), m_files.end(), [](const auto& file) { return !file; });
                if (empty == m_files.end()) {
                    if (m_files.size() == UINT32_MAX) throw std::bad_alloc{};
                    m_files.emplace_back();
                    empty = m_files.end() - 1;
                }
                auto& record = *request->candidate;
                record.sourceGeneration = ++m_lastSourceGeneration;
                if (previous) {
                    previous->retiring = true;
                    previous->retiredFromId = previous->soundId;
                    previous->soundId = {};
                }
                if (out) {
                    out->fileIndex = static_cast<std::uint32_t>(empty - m_files.begin());
                    out->sourceGeneration = record.sourceGeneration;
                    out->contentRevision = record.contentRevision;
                    out->sampleRate = record.toc.sampleRate;
                    out->channelCount = record.toc.channelCount;
                    out->frameCount = record.toc.frameCount;
                    out->flags = record.toc.flags;
                    out->loopStartFrame = record.toc.loopStartFrame;
                    out->loopEndFrame = record.toc.loopEndFrame;
                }
                *empty = std::move(request->candidate);
                request.reset();
                m_serviceRequested = true;
                m_wake.notify_all();
                return VN_OK;
            } catch (const std::bad_alloc&) { refusal = VN_BACKPRESSURE; }
        }
        if (refusal == VN_BACKPRESSURE || (retryable && refusal == VN_IN_USE)) return refusal;
        request->state = PreparationState::Failed;
        request->result = refusal;
        return refusal;
    }
    return VN_STALE_HANDLE;
}

VnResult StreamManager::RegisterSourceAndWait(VnSoundId id, const char* path, bool replace) {
    StreamFacts current;
    if (replace && !FindFile(id, &current))
        return WorkerState() == StreamWorkerState::Ready ? VN_UNKNOWN_ID : VN_DEVICE_ERROR;
    SourceToken token;
    const auto accepted = BeginFileSource(id, path, replace, current.sourceGeneration, &token);
    if (accepted != VN_OK) return accepted;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            bool preparing = false;
            for (const auto& request : m_sourceRequests)
                if (request && request->generation == token.value)
                    preparing = request->state == PreparationState::Preparing;
            if (!preparing || WorkerState() != StreamWorkerState::Ready || m_shutdown) break;
            m_done.wait_for(lock, kIoPoll);
        }
    }
    const auto result = WorkerState() == StreamWorkerState::Ready ? TakeSource(token) : VN_DEVICE_ERROR;
    if (result != VN_OK) CancelSource(token);
    return result;
}

void StreamManager::QueueCleanup(std::unique_ptr<StreamFile> file) noexcept {
    if (!file) return;
    file->nextCleanup = std::move(m_cleanup);
    m_cleanup = std::move(file);
}

void StreamManager::DrainCleanupAfterStop() noexcept {
    // All other lanes and the manager have joined before this final owner runs.
    decltype(m_files) files;
    decltype(m_sourceRequests) requests;
    std::unique_ptr<StreamFile> cleanup;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        files.swap(m_files);
        requests.swap(m_sourceRequests);
        cleanup = std::move(m_cleanup);
    }
    while (cleanup) {
        auto file = std::move(cleanup);
        cleanup = std::move(file->nextCleanup);
    }
}

void StreamManager::StopIo() noexcept {
    for (auto& lane : m_ioLanes) lane->cancelled.store(true, std::memory_order_release);
    for (auto& slot : m_slots) slot.io.cancelled.store(true, std::memory_order_release);
    for (auto& request : m_sourceRequests)
        if (request) request->cancelled.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_ioMutex);
        m_ioStop.store(true, std::memory_order_release);
    }
    m_ioWake.notify_all();
    m_steadyWake.notify_all();
    for (std::size_t i = 1; i < m_ioLanes.size(); ++i)
        if (m_ioLanes[i]->thread.joinable()) m_ioLanes[i]->thread.join();
    {
        std::lock_guard<std::mutex> lock(m_ioMutex);
        m_ioShutdownReady.store(true, std::memory_order_release);
    }
    m_ioWake.notify_all();
    if (!m_ioLanes.empty() && m_ioLanes[0]->thread.joinable()) m_ioLanes[0]->thread.join();
}

void StreamManager::IoMain(std::size_t index) {
    if (index >= static_cast<std::size_t>(kSteadyIoOwner)) { SteadyIoMain(); return; }
    auto& lane = *m_ioLanes[index];
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_ioMutex);
            m_ioWake.wait(lock, [&] {
                return m_ioStop.load(std::memory_order_acquire) ||
                       lane.state.load(std::memory_order_acquire) == 1;
            });
            if (lane.state.load(std::memory_order_acquire) != 1) break;
            lane.state.store(2, std::memory_order_relaxed);
        }
        lane.exception = false;
        try {
            switch (lane.kind) {
            case IoLane::Kind::Chunk:
                lane.valid = RunChunkJob(lane.chunk, lane.slot->staging, lane.slot->window, lane.readFacts);
                break;
            case IoLane::Kind::Source:
                ValidateSourceSlice(*lane.request);
                break;
            case IoLane::Kind::Cleanup:
                lane.cleanup.reset();
                break;
            }
        } catch (const std::bad_alloc&) {
            lane.exception = true;
            if (lane.kind == IoLane::Kind::Source) lane.request->result = VN_BACKPRESSURE;
        } catch (...) {
            lane.exception = true;
            if (lane.kind == IoLane::Kind::Source) lane.request->result = VN_BAD_STREAM_FILE;
        }
        lane.state.store(3, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_serviceRequested = true;
        }
        m_wake.notify_all();
    }
    if (index == 0) {
        std::unique_lock<std::mutex> lock(m_ioMutex);
        m_ioWake.wait(lock, [&] { return m_ioShutdownReady.load(std::memory_order_acquire); });
        lock.unlock();
        DrainCleanupAfterStop();
    }
}

void StreamManager::SteadyIoMain() {
    for (;;) {
        StreamSlot* slot = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_ioMutex);
            m_steadyWake.wait(lock, [&] {
                return m_ioStop.load(std::memory_order_acquire) || !m_readySteady.empty();
            });
            if (m_readySteady.empty()) return;
            slot = m_readySteady.front();
            m_readySteady.pop_front();
        }
        auto& job = slot->io;
        job.state.store(2, std::memory_order_relaxed);
        job.exception = false;
        job.valid = false;
        job.readFacts = {};
        try {
            if (!job.cancelled.load(std::memory_order_acquire))
                job.valid = RunChunkJob(job.input, slot->staging, slot->window, job.readFacts);
        } catch (...) { job.exception = true; }
        job.state.store(3, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_serviceRequested = true;
        }
        m_wake.notify_one();
        // The next independent slot needs no manager round trip. This job's
        // buffer remains owned by its slot until the manager consumes it.
    }
}

void StreamManager::SettleIo() {
    for (auto& pointer : m_ioLanes) {
        auto& lane = *pointer;
        if (lane.state.load(std::memory_order_acquire) != 3) continue;
        if (lane.kind == IoLane::Kind::Chunk) {
            auto& slot = *lane.slot;
            CompleteReadObservation(slot, lane.chunk, lane.readFacts, lane.valid, lane.exception);
            if (slot.ring.state.load(std::memory_order_acquire) == StreamSlotState::Active &&
                !lane.cancelled.load(std::memory_order_acquire)) {
                if (lane.exception) {
                    ++slot.exceptionSerial;
                    slot.passGoal = 0;
                } else CompleteChunk(slot, lane.chunk, lane.valid);
            }
            slot.ioLane = -1;
            lane.slot = nullptr;
        } else if (lane.kind == IoLane::Kind::Source) {
            auto& request = *lane.request;
            request.ioLane = -1;
            if (WorkerState() == StreamWorkerState::Failed) request.result = VN_DEVICE_ERROR;
            if (!request.discard && request.state == PreparationState::Preparing) {
                if (request.result != VN_OK) request.state = PreparationState::Failed;
                else if (request.phase == 3) request.state = PreparationState::Ready;
            }
            lane.request = nullptr;
            m_done.notify_all();
        }
        lane.state.store(0, std::memory_order_release);
    }
    for (auto& slot : m_slots) {
        if (slot.io.state.load(std::memory_order_acquire) != 3) continue;
        CompleteReadObservation(slot, slot.io.input, slot.io.readFacts, slot.io.valid, slot.io.exception);
        if (slot.ring.state.load(std::memory_order_acquire) == StreamSlotState::Active &&
            !slot.io.cancelled.load(std::memory_order_acquire)) {
            if (slot.io.exception) {
                ++slot.exceptionSerial;
                slot.passGoal = 0;
            } else CompleteChunk(slot, slot.io.input, slot.io.valid);
        }
        slot.ioLane = -1;
        slot.io.state.store(0, std::memory_order_release);
    }
    for (auto& request : m_sourceRequests) {
        if (request && request->discard && request->ioLane < 0) {
            QueueCleanup(std::move(request->candidate));
            request.reset();
        }
    }
}

void StreamManager::ServiceRequests() {
    for (auto iterator = m_batch.begin(); iterator != m_batch.end();) {
        auto& request = **iterator;
        if (request.kind == Request::Kind::Unregister) {
            HandleUnregister(request);
            request.done = true;
        } else if (request.kind == Request::Kind::Reap) {
            // Settle/detach/close already ran before request processing.
            request.done = true;
        } else {
            if (!request.started) {
                request.targets.reserve(m_slots.size());
                for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
                    auto& slot = m_slots[i];
                    if (request.onlySlot != UINT32_MAX &&
                        (i != request.onlySlot || slot.ring.generation.load(std::memory_order_relaxed) !=
                         request.onlyGeneration)) continue;
                    if (slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active) continue;
                    const auto frames = (!m_throttled.load(std::memory_order_relaxed) || request.forceServe)
                        ? BeginSlotService(slot).maxServeFrames : 0u;
                    request.targets.push_back({i, slot.ring.generation.load(std::memory_order_relaxed),
                        slot.ring.writeFrames.load(std::memory_order_relaxed) + frames, slot.exceptionSerial});
                }
                request.started = true;
            }
            request.done = true;
            for (const auto& target : request.targets) {
                const auto& slot = m_slots[target.slot];
                if (slot.ring.generation.load(std::memory_order_relaxed) != target.generation ||
                    slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active) continue;
                if (slot.exceptionSerial != target.exceptionSerial) request.result = VN_BAD_STREAM_FILE;
                else if (slot.ioLane >= 0 || (!slot.feedStopped &&
                         slot.ring.writeFrames.load(std::memory_order_relaxed) < target.writeIndex)) request.done = false;
            }
            if (request.result != VN_OK) request.done = true;
        }
        if (request.done) { iterator = m_batch.erase(iterator); m_done.notify_all(); }
        else ++iterator;
    }
}

void StreamManager::ServeSlots() {
    std::size_t cachedBytesRemaining = ReadWindow::kCapacityBytes;
    for (auto& slot : m_slots) {
        if (slot.ioLane >= 0 || slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active) continue;
        const auto generation = slot.ring.generation.load(std::memory_order_relaxed);
        auto cursor = BeginSlotService(slot);
        const auto written = slot.ring.writeFrames.load(std::memory_order_relaxed);
        if (slot.passGoal <= written) {
            std::uint64_t target = written;
            if (!m_throttled.load(std::memory_order_relaxed)) target += cursor.maxServeFrames;
            const auto index = static_cast<std::uint32_t>(&slot - m_slots.data());
            for (const auto* request : m_batch)
                if (request->kind == Request::Kind::Serve)
                    for (const auto& wanted : request->targets)
                        if (wanted.slot == index && wanted.generation == slot.ring.generation.load(std::memory_order_relaxed))
                            target = std::max(target, wanted.writeIndex);
            slot.passGoal = target;
        }
        cursor.maxServeFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            cursor.maxServeFrames, slot.passGoal > written ? slot.passGoal - written : 0));
        ChunkJob job;
        // A steady window hit needs no executor or allocation. Keep the same
        // finite cursor so newly verified staging is published in this pass;
        // misses and exhausted byte budget retain the normal dispatch path.
        while (cursor.maxServeFrames != 0 && AdvanceSlot(cursor, &job)) {
            if (!slot.ring.startPromoted.load(std::memory_order_acquire) ||
                job.chunkBytes > cachedBytesRemaining ||
                slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
                slot.ring.generation.load(std::memory_order_relaxed) != generation ||
                !TryCopyReadWindow(job, slot.staging, slot.window)) break;
            cachedBytesRemaining -= static_cast<std::size_t>(job.chunkBytes);
            slot.stagedChunk = kNoChunk;
            const bool valid = ValidateChunk(job, slot.staging,
                Fnv1a64(slot.staging.data(), static_cast<std::size_t>(job.chunkBytes)));
            if (slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
                slot.ring.generation.load(std::memory_order_relaxed) != generation) break;
            CompleteReadObservation(slot, job, {}, valid, false);
            CompleteChunk(slot, job, valid);
            if (!valid) break;
        }
    }
}

void StreamManager::DispatchIo() {
    if (m_ioLanes.empty()) return;
    auto dispatch = [&](IoLane& lane) {
        {
            std::lock_guard<std::mutex> lock(m_ioMutex);
            lane.cancelled.store(false, std::memory_order_relaxed);
            lane.state.store(1, std::memory_order_release);
        }
        // The two mailboxes share this condition variable but have different
        // predicates. Wake both so the owner of this job always observes it.
        m_ioWake.notify_all();
    };
    auto& sourceLane = *m_ioLanes[kSourceIoLane];
    if (sourceLane.state.load(std::memory_order_acquire) == 0 && m_cleanup) {
        sourceLane.kind = IoLane::Kind::Cleanup;
        sourceLane.cleanup = std::move(m_cleanup);
        m_cleanup = std::move(sourceLane.cleanup->nextCleanup);
        dispatch(sourceLane);
    }
    if (WorkerState() != StreamWorkerState::Ready) return;
    auto source = [&] {
        if (sourceLane.state.load(std::memory_order_acquire) != 0) return false;
        for (std::size_t n = 0; n < m_sourceRequests.size(); ++n) {
            const auto i = (m_sourceCursor + n) % m_sourceRequests.size();
            auto& request = m_sourceRequests[i];
            if (!request || request->discard || request->ioLane >= 0 ||
                request->state != PreparationState::Preparing) continue;
            request->ioLane = kSourceIoLane;
            sourceLane.kind = IoLane::Kind::Source;
            sourceLane.request = request.get();
            m_sourceCursor = i + 1;
            dispatch(sourceLane);
            return true;
        }
        return false;
    };
    (void)source();
    if (m_slots.empty()) return;
    auto& prepare = *m_ioLanes[kPreparationIoLane];
    std::size_t queuedSteady = 0;
    for (std::size_t choice = 0; choice <= m_slots.size(); ++choice) {
        const bool steady = choice != 0;
        if (!steady && prepare.state.load(std::memory_order_acquire) != 0) continue;
        StreamSlot* selected = nullptr;
        double leastTime = std::numeric_limits<double>::infinity();
        for (std::size_t n = 0; n < m_slots.size(); ++n) {
            auto& slot = m_slots[(m_slotCursor + n) % m_slots.size()];
            if (slot.ioLane >= 0 || slot.feedStopped ||
                slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
                slot.ring.startPromoted.load(std::memory_order_acquire) != steady ||
                slot.passGoal <= slot.ring.writeFrames.load(std::memory_order_relaxed)) continue;
            const double time = static_cast<double>(slot.ring.BufferedFrames()) /
                                m_files[slot.fileIndex]->toc.sampleRate;
            if (time < leastTime) { selected = &slot; leastTime = time; }
        }
        if (!selected) {
            // Remaining steady choices observe the same eligibility set.
            // The preparation choice must still yield to steady work.
            if (steady) break;
            continue;
        }
        auto& slot = *selected;
        auto cursor = BeginSlotService(slot);
        cursor.maxServeFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            cursor.maxServeFrames, slot.passGoal - slot.ring.writeFrames.load(std::memory_order_relaxed)));
        ChunkJob job;
        if (!AdvanceSlot(cursor, &job)) continue;
        slot.stagedChunk = kNoChunk;
        m_slotCursor = static_cast<std::size_t>(&slot - m_slots.data()) + 1;
        if (steady) {
            std::lock_guard<std::mutex> lock(m_ioMutex);
            // Queue insertion is the only fallible step. No ownership is
            // published if it fails; the manager can retry that missing chunk.
            m_readySteady.push_back(&slot);
            slot.ioLane = kSteadyIoOwner;
            slot.io.cancelled.store(false, std::memory_order_relaxed);
            job.cancellation = {&slot.io.cancelled};
            slot.io.input = job;
            slot.io.state.store(1, std::memory_order_release);
            if (queuedSteady == 0) m_steadyWake.notify_one();
            ++queuedSteady;
        } else {
            slot.ioLane = kPreparationIoLane;
            prepare.kind = IoLane::Kind::Chunk;
            prepare.slot = &slot;
            job.cancellation = {&prepare.cancelled};
            prepare.chunk = job;
            dispatch(prepare);
        }
    }
    if (queuedSteady > 1) m_steadyWake.notify_one();
}

} // namespace harken
