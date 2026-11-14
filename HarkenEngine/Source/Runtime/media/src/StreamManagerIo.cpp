#include "harken/media/StreamManager.h"
#include "harken/media/VorbisDecoder.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

namespace harken {
namespace {
constexpr auto kIoPoll = std::chrono::milliseconds(2);
}

StreamManager::PreparationState StreamManager::PreparePackSource(std::uint32_t index, std::uint64_t generation,
    VorbisCodec& codec, std::size_t budget, VnResult* error) {
    if (error) *error = VN_OK;
    const auto failed = [&](VnResult result) {
        if (error) *error = result;
        return PreparationState::Failed;
    };
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || WorkerState() != StreamWorkerState::Ready) return failed(VN_DEVICE_ERROR);
    if (index >= m_files.size() || !m_files[index] ||
        m_files[index]->sourceGeneration != generation) return failed(VN_STALE_HANDLE);
    auto& file = *m_files[index];
    if (file.MetadataReady()) return PreparationState::Ready;
    if (file.metadataResult != VN_OK) return failed(file.metadataResult);
    for (auto& request : m_sourceRequests) {
        if (!request || request->metadataFile != &file) continue;
        if (request->discard || request->ioLane >= 0 ||
            request->state == PreparationState::Preparing) return PreparationState::Preparing;
        VnResult result = request->result;
        if (request->state == PreparationState::Ready) {
            try {
                if (file.external->codec == PackExternalStream::Codec::Pcm) {
                    if (AdoptPcmMetadata(file, *request)) {
                        file.source = std::move(request->candidate->source);
                        result = VN_OK;
                    } else result = VN_BAD_STREAM_FILE;
                } else {
                    VorbisPrepareResult prepared;
                    auto media = VorbisStreamMedia::Prepare(codec, std::move(request->metadataPrefix),
                        file.external->fileBytes - file.packBlobOffset, request->metadataBudget, prepared);
                    result = prepared == VorbisPrepareResult::Unsupported ? VN_CAPABILITY_UNAVAILABLE :
                        prepared == VorbisPrepareResult::OutOfMemory ? VN_BACKPRESSURE : VN_BAD_STREAM_FILE;
                    if (media) {
                        const auto& facts = media->Media();
                        bool valid = facts.sampleRate == file.toc.sampleRate && facts.channels == file.toc.channelCount &&
                            facts.frames == file.toc.frameCount && facts.revision == file.contentRevision &&
                            facts.loopStart == file.toc.loopStartFrame && facts.loopEnd == file.toc.loopEndFrame;
                        for (std::size_t i = 0; valid && i < facts.seek.size(); ++i) {
                            const auto end = i + 1 < facts.seek.size() ? facts.seek[i + 1].packetOffset : facts.packetBytes;
                            valid = end - facts.seek[i].packetOffset <=
                                std::uint64_t(kMaxStreamChunkFrames) * facts.channels * sizeof(float);
                        }
                        if (valid) {
                            // No slot can start before metadata is published. The
                            // source request's pin excludes idle close throughout.
                            file.chunkChecksums = facts.segmentChecksums;
                            file.vorbis = std::move(media);
                            file.source = std::move(request->candidate->source);
                            result = VN_OK;
                        }
                    }
                }
            } catch (const std::bad_alloc&) { result = VN_BACKPRESSURE; }
        }
        file.metadataResult = result;
        file.pendingRefs.fetch_sub(1, std::memory_order_relaxed);
        QueueCleanup(std::move(request->candidate));
        request.reset();
        m_serviceRequested = true;
        m_wake.notify_all();
        return result == VN_OK ? PreparationState::Ready : failed(result);
    }
    if (file.pendingRefs.load(std::memory_order_relaxed) == 0 &&
        file.activeStreams.load(std::memory_order_relaxed) == 0) return failed(VN_INVALID_ARG);
    try {
        auto request = std::make_unique<SourceRequest>();
        request->candidate = std::make_unique<StreamFile>();
        request->metadataFile = &file;
        request->metadataBudget = budget;
        auto empty = std::find_if(m_sourceRequests.begin(), m_sourceRequests.end(),
            [](const auto& item) { return !item; });
        if (empty == m_sourceRequests.end()) {
            m_sourceRequests.emplace_back();
            empty = m_sourceRequests.end() - 1;
        }
        file.pendingRefs.fetch_add(1, std::memory_order_relaxed);
        *empty = std::move(request);
        m_serviceRequested = true;
        m_wake.notify_all();
        return PreparationState::Preparing;
    } catch (const std::bad_alloc&) { return failed(VN_BACKPRESSURE); }
}

void StreamManager::ReadExternalMetadataSlice(SourceRequest& request) {
    const auto& file = *request.metadataFile;
    auto& source = request.candidate->source;
    const StreamReadCancellation cancel{&request.cancelled};
    if (cancel.IsCancelled()) return;
    if (file.external->codec == PackExternalStream::Codec::Pcm) {
        if (!source) {
            auto opened = file.external->locator->Open(cancel);
            std::array<std::uint8_t, vorbis_test_wire::kStreamHeaderBytes> header{};
            if (!opened || opened->ByteCount() != file.external->fileBytes ||
                ReadStreamBytes(*opened, 0, header.data(), header.size(), cancel) != StreamReadStatus::Ok ||
                !ValidateTestStreamHeader(header.data(), header.size(), file.external->fileBytes, 0)) {
                request.result = VN_BAD_STREAM_FILE; return;
            }
            source = StreamByteRange(std::move(opened), file.packBlobOffset,
                file.external->fileBytes - file.packBlobOffset);
            return;
        }
        ValidatePcmSourceSlice(request);
        return;
    }
    if (request.phase == 0) {
        source = file.external->locator->Open(cancel);
        std::array<std::uint8_t, vorbis_test_wire::kStreamHeaderBytes> header{};
        if (!source || source->ByteCount() != file.external->fileBytes ||
            ReadStreamBytes(*source, 0, header.data(), header.size(), cancel) != StreamReadStatus::Ok ||
            !ValidateTestStreamHeader(header.data(), header.size(), file.external->fileBytes)) {
            request.result = VN_BAD_STREAM_FILE; return;
        }
        request.phase = 1;
        return;
    }
    if (request.phase == 1) {
        std::array<std::uint8_t, vorbis_test_wire::kHeaderBytes> header{};
        if (ReadStreamBytes(*source, file.packBlobOffset, header.data(), header.size(), cancel) != StreamReadStatus::Ok) {
            request.result = VN_BAD_STREAM_FILE; return;
        }
        const auto bytes = VorbisTestMetadataBytes(header.data(), header.size());
        if (bytes < header.size() || bytes > file.external->fileBytes - file.packBlobOffset || bytes > SIZE_MAX) {
            request.result = VN_BAD_STREAM_FILE; return;
        }
        if (bytes > request.metadataBudget) { request.result = VN_BACKPRESSURE; return; }
        request.metadataPrefix.resize(static_cast<std::size_t>(bytes));
        std::copy(header.begin(), header.end(), request.metadataPrefix.begin());
        request.metadataRead = header.size();
        request.metadataBytes.store(sizeof(StreamFile) + request.metadataPrefix.capacity(), std::memory_order_release);
        request.phase = 2;
        return;
    }
    const auto count = std::min(ReadWindow::kCapacityBytes, request.metadataPrefix.size() - request.metadataRead);
    if (count && ReadStreamBytes(*source, file.packBlobOffset + request.metadataRead,
            request.metadataPrefix.data() + request.metadataRead, count, cancel) != StreamReadStatus::Ok) {
        request.result = VN_BAD_STREAM_FILE; return;
    }
    request.metadataRead += count;
    if (request.metadataRead == request.metadataPrefix.size()) request.phase = 3;
}

void StreamManager::ServicePackSources(VorbisCodec& codec, std::size_t budget) {
    for (std::uint32_t index = 0; index < m_slots.size(); ++index) {
        std::uint32_t fileIndex = 0, ringGeneration = 0;
        std::uint64_t sourceGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto& slot = m_slots[index];
            if (!slot.metadataWaiting || slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active)
                continue;
            fileIndex = slot.fileIndex;
            ringGeneration = slot.ring.generation.load(std::memory_order_relaxed);
            sourceGeneration = slot.ring.sourceGeneration;
        }
        VnResult error = VN_OK;
        const auto state = PreparePackSource(fileIndex, sourceGeneration, codec, budget, &error);
        if (state == PreparationState::Preparing) continue;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& slot = m_slots[index];
        if (!slot.metadataWaiting || slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active ||
            slot.ring.generation.load(std::memory_order_relaxed) != ringGeneration ||
            slot.ring.sourceGeneration != sourceGeneration) continue;
        if (state == PreparationState::Ready) {
            auto& file = *m_files[fileIndex];
            if (file.PacketEncoded()) {
                MediaDecoder* decoder = file.vorbis ? file.vorbis->Acquire() : nullptr;
                if (decoder) {
                    ResetPacketCursor(slot, file);
                    slot.ring.pendingDecoder.store(decoder, std::memory_order_release);
                } else error = VN_NO_VOICE;
            }
        }
        slot.metadataWaiting = false;
        if (error != VN_OK) {
            slot.feedStopped = true;
            slot.ring.preparationResult.store(error, std::memory_order_release);
            slot.ring.MarkEndOfData(error == VN_BAD_STREAM_FILE ? StreamTerminalReason::IntegrityAbort :
                StreamTerminalReason::ProducerFailure);
        }
        m_serviceRequested = true;
        m_wake.notify_all();
    }
}

std::uint64_t StreamManager::SourcePreparationBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uint64_t bytes = m_sourceRequests.capacity() * sizeof(std::unique_ptr<SourceRequest>);
    for (const auto& request : m_sourceRequests) {
        if (!request) continue;
        bytes += sizeof(SourceRequest);
        bytes += request->metadataBytes.load(std::memory_order_acquire);
    }
    return bytes;
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
                ReadExternalMetadataSlice(*lane.request);
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
        if (request && request->metadataFile && !request->discard &&
            request->metadataFile->pendingRefs.load(std::memory_order_relaxed) == 1 &&
            request->metadataFile->activeStreams.load(std::memory_order_relaxed) == 0) {
            request->discard = true;
            request->cancelled.store(true, std::memory_order_release);
        }
        if (request && request->discard && request->ioLane < 0) {
            if (request->metadataFile)
                request->metadataFile->pendingRefs.fetch_sub(1, std::memory_order_relaxed);
            QueueCleanup(std::move(request->candidate));
            request.reset();
        }
    }
}

void StreamManager::ServiceRequests() {
    for (auto iterator = m_batch.begin(); iterator != m_batch.end();) {
        auto& request = **iterator;
        if (request.kind == Request::Kind::Reap) {
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
                if (slot.ring.preparationResult.load(std::memory_order_acquire) != VN_OK)
                    request.result = slot.ring.preparationResult.load(std::memory_order_relaxed);
                else if (slot.exceptionSerial != target.exceptionSerial) request.result = VN_BAD_STREAM_FILE;
                else if (slot.ioLane >= 0 || (!slot.feedStopped && !slot.packetCapacityLimited &&
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
        if (slot.ioLane >= 0 || slot.metadataWaiting ||
            slot.ring.state.load(std::memory_order_acquire) != StreamSlotState::Active) continue;
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
                Fnv1a64(slot.staging.data(), static_cast<std::size_t>(job.chunkBytes),
                    job.file->vorbis ? job.file->vorbis->Media().segmentChecksumSeed : kFnv1a64OffsetBasis));
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
            if (slot.ioLane >= 0 || slot.metadataWaiting || slot.feedStopped || slot.packetCapacityLimited ||
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
