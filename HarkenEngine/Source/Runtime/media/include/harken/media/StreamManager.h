// Stream manager: registered .stream files, the stream slot table, and
// the streaming thread that owns ALL file I/O.
//
// Thread roles are strict. The streaming thread ("HarkenStreaming") opens,
// validates, reads, and closes every file — registration validation
// included: the logic thread posts a request and blocks until served, so
// no file handle ever exists outside the streaming thread. The audio
// thread touches nothing here; its entire contact surface is the per-slot
// StreamRing (SPSC frames plus the slot lifecycle word), so it never
// shares a lock with either of the other threads. The logic thread and
// the streaming thread may share the request mutex freely — only the
// audio thread is under the no-locks rule.
//
// Slot lifecycle: the logic thread claims a Free slot at play time, wires
// it (ring reset, file binding, read cursor), and publishes Active; the
// audio thread stores Retired when the voice releases; the streaming
// thread detaches Retired slots (drops its cursor state, releases the
// file's stream count) and returns them to Free.
//
// Ring addresses are reserved at construction. Logic prepares the required
// slot/channel planes before accepting playback or a pending reference;
// activation never grows storage and active planes never move.
#ifndef HARKEN_MEDIA_STREAM_MANAGER_H
#define HARKEN_MEDIA_STREAM_MANAGER_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/core/StableRowStorage.h"
#include "harken/core/StreamRing.h"
#include "harken/media/StreamFormat.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The Windows SDK aliases ReplaceFile to its encoding-specific entry point.
#ifdef ReplaceFile
#undef ReplaceFile
#endif

namespace harken {

namespace detail { struct StreamManagerTestAccess; }

// Deterministic streaming-thread failures for internal boundary tests.
// Enqueue fails the request handoff itself; WorkerStartup makes the streaming
// thread die before it can serve anything; WorkerLoop makes it die after it has
// begun serving, with requests already in flight.
// WorkerStall holds the streaming thread inside its dispatch region, where it has
// released the request mutex. That is the one window in which a request can be
// observed sitting in the queue while the thread is not consuming it, which is
// what a shutdown racing a queued request needs in order to be tested at all
// rather than raced for.
enum class StreamRequestFault : std::uint32_t {
    None = 0,
    Enqueue = 1,
    WorkerStartup = 2,
    WorkerLoop = 3,
    WorkerStall = 4
};

void SetStreamRequestFaultForTests(StreamRequestFault fault) noexcept;
StreamRequestFault StreamRequestFaultForTests() noexcept;

// Releases a streaming thread held by WorkerStall. Set false before arming.
void SetStreamWorkerStallReleaseForTests(bool release) noexcept;
// Counts how many times the streaming thread has entered the stall, so a test
// can wait for the window to actually open instead of sleeping and hoping.
std::uint32_t StreamWorkerStallEntriesForTests() noexcept;

// Streaming-thread lifecycle, published by that thread and read by the logic
// thread. The audio thread never reads it: its only contact with streaming is
// the per-slot ring, and a dead streaming thread reaches it as end-of-data on
// the rings it was feeding, not as a state to interpret.
//
// Ready is published before the service loop begins accepting work, so a caller
// that observes it knows the thread got far enough to serve. Failed is terminal
// and one-way: the thread has stopped and will never serve again, so requests
// are refused instead of queued behind it. Stopping and Stopped mark ordinary
// teardown.
enum class StreamWorkerState : std::uint32_t {
    Starting = 0,
    Ready = 1,
    Failed = 2,
    Stopping = 3,
    Stopped = 4
};

class StreamManager {
public:
    explicit StreamManager(const EngineConfig& config);
    ~StreamManager();

    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    // Registers a .stream file under soundId. Blocks while the
    // streaming thread opens the file and verifies the header, TOC, and
    // chunk-checksum table (the table's own bytes are covered by the TOC
    // checksum; per-chunk payload verification happens later, as chunks
    // are read). The file stays open until unregistration. VN_IN_USE for
    // an id already registered here; VN_BAD_STREAM_FILE for an
    // unreadable, malformed, or checksum-failing file. Logic thread only.
    VnResult RegisterFile(VnSoundId soundId, const char* utf8Path);

    // Removes a registration and closes the file (served by the streaming
    // thread; blocks like RegisterFile). VN_UNKNOWN_ID when the id names
    // no registered stream; VN_IN_USE while any stream slot still plays
    // it. Logic thread only.
    VnResult UnregisterFile(VnSoundId soundId);

    // Hot reload: rebinds a registered stream id to a new .stream file. The
    // new file opens and validates end to end on the streaming thread (the
    // RegisterFile contract, including entry.soundId == soundId); any
    // failure is VN_BAD_STREAM_FILE with the old registration untouched.
    // VN_UNKNOWN_ID when the id names no registered stream; VN_IN_USE while
    // a previous replace of this id still has a retiring file open (one
    // retiring generation per asset). On success the id resolves to the new
    // file immediately, while the old file stays open as a retiring
    // generation: slots already active on it (and pending-start pins) keep
    // reading it by file index, and the streaming thread closes it once its
    // active and pending counts both drain. Logic thread only.
    VnResult ReplaceFile(VnSoundId soundId, const char* utf8Path);

    // Play-time facts of a registered stream, resolved on the logic side
    // exactly like pack loop metadata — the audio thread never looks
    // anything up.
    struct StreamFacts {
        std::uint32_t fileIndex = 0;
        std::uint32_t sampleRate = 0;
        std::uint32_t channelCount = 0;
        std::uint64_t frameCount = 0;
        std::uint64_t contentRevision = 0;
        std::uint32_t flags = 0;
        std::uint64_t loopStartFrame = 0;
        std::uint64_t loopEndFrame = 0;
    };

    // Logic thread. False when the id names no registered stream. A
    // retiring file (replaced, still draining) is not registered under any
    // id and never matches here.
    bool FindFile(VnSoundId soundId, StreamFacts* out) const;

    // Facts by file index rather than id — the generation-pinned lookup a
    // deferred stream start uses so it begins on the file it was admitted
    // against, even if the id was replaced while it waited. Works for
    // retiring files. False when the index holds no open file. Logic thread
    // only.
    bool FactsForFileIndex(std::uint32_t fileIndex, StreamFacts* out) const;

    // Retiring files still open (the stream share of the snapshot's
    // pinnedGenerationCount). Logic thread.
    std::uint32_t RetiringFileCount() const;

    // Whether a retiring file that was replaced away from this id is still
    // open. Logic thread.
    bool HasRetiringFor(VnSoundId soundId) const;

    // Pins a registered stream against unregistration while a steal's pending-
    // start voice waits for a render slot. A pending stream voice holds no
    // stream slot, so without a reference the source could be unregistered
    // (dropping the deferred start) or unregistered and re-registered to a
    // different asset under the same id (starting the wrong source). While any
    // pending reference is held, UnregisterFile refuses VN_IN_USE, which also
    // blocks re-registration of the id — so both the source and its exact
    // registration survive the pending window. fileIndex comes from
    // StreamFacts. Balanced by ReleasePendingRef at start (handed off to the
    // active stream's own count) or cancel. False if the index no longer names
    // a registered file. Logic thread only.
    bool AcquirePendingRef(std::uint32_t fileIndex);
    void ReleasePendingRef(std::uint32_t fileIndex);

    // Side-effect-free capacity peek: the index of a Free slot, or false
    // when every slot is taken. The API is externally synchronized and
    // only the logic thread claims, so a peeked slot stays Free until
    // ActivateSlot. Logic thread only.
    bool PeekFreeSlot(std::uint32_t* outSlot) const;

    // Prepare actual channel planes before an unaccepted start publishes a slot.
    bool PrepareSlotWorkingSet(std::uint32_t slot, std::uint32_t channels) noexcept;
    // A pending reference already holds the backing. Reap private rollback
    // rings if necessary, without growing storage after acceptance.
    bool PeekPreparedFreeSlot(std::uint32_t channels, std::uint32_t* outSlot);

    // Number of currently Free stream slots. The admission paths compare this
    // against the outstanding pending-start reservations so promised capacity
    // is never handed to a second claimant. Logic thread only.
    std::uint32_t FreeSlotCount() const;

    // The slot's ring, for wiring into the voice render context.
    StreamRing* RingOf(std::uint32_t slot);

    // Publishes a peeked slot as Active: resets the ring for the file's
    // channel count, binds the file, seeds the read cursor to startFrame (0 =
    // the file start, the original behavior; a nonzero value seeks there via the
    // serve loop's chunk-aligned read, honored by the first prebuffer fill), then
    // stores Active (release) so the streaming thread starts feeding it. Logic
    // thread only, after the voice context is fully wired.
    VnResult ActivateSlot(std::uint32_t slot, std::uint32_t fileIndex,
                          std::uint64_t startFrame,
                          std::uint64_t loopStartFrame,
                          std::uint64_t loopEndFrame,
                          std::uint32_t loopCount,
                          std::uint64_t* outRingToken = nullptr);

    // Prepares one replacement ring for an absolute, single-pass seek. The
    // returned token is slot-and-generation exact; the caller owns the Active
    // ring until it is transferred to a voice or retired.
    VnResult PrepareSeekSlot(std::uint32_t fileIndex,
                             std::uint64_t contentRevision,
                             std::uint64_t targetFrame,
                             std::uint32_t prebufferTargetFrames,
                             std::uint32_t* outSlot,
                             StreamRing** outRing,
                             std::uint64_t* outRingToken);
    bool RetirePreparedSlot(StreamRing* ring,
                            std::uint64_t ringToken) noexcept;

    static std::uint64_t RingToken(std::uint32_t slot,
                                   std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 32u) | slot;
    }

    // Observability facts merged into snapshots at capture time (logic
    // thread): most slots ever Active at once, and the fullest any ring
    // has ever been (frames), measured by the producer after each write.
    std::uint64_t StreamHighWater() const { return m_streamHighWater; }
    std::uint64_t RingHighWaterFrames() const {
        return m_ringHighWaterFrames.load(std::memory_order_relaxed);
    }

    // --- deterministic test hooks ---
    // Throttled, the streaming thread still serves requests and detaches
    // retired slots but pushes no frames — rings drain and stay dry, so a
    // starvation drill is exact, not timing-luck. SyncServe posts a
    // request and blocks until one full service pass has run on the
    // streaming thread, so tests order "ring now filled" / "ring still
    // dry" against block pumping without sleeps.
    void SetThrottled(bool throttled) {
        m_throttled.store(throttled, std::memory_order_relaxed);
    }
    void SetMaxServeFramesForTest(std::uint32_t frames) {
        m_maxServeFramesForTest.store(frames, std::memory_order_relaxed);
    }
    // The next ActivateSlot seeds its read cursor to this frame instead of the
    // startFrame it is passed, then clears the override. This drives exact seek
    // independently of the stream-park acknowledgement path, the same way the
    // throttle and serve-budget hooks drive other stream facts.
    void SetActivationStartFrameForTest(std::uint64_t frame) {
        m_testActivationStartFrame.store(frame, std::memory_order_relaxed);
        m_testActivationStartFrameSet.store(true, std::memory_order_relaxed);
    }
    void SyncServe();

    // Requests queued but not yet claimed by the streaming thread. Exists so a
    // test can prove a request that unwound early left nothing behind: its
    // Request lives on the caller's stack, so a stale pointer left queued would
    // be dereferenced by whoever drains the queue next.
    std::size_t PendingRequestCountForTest() const;

    // Logic thread. The streaming thread's own view of its lifecycle.
    StreamWorkerState WorkerState() const noexcept {
        return m_workerState.load(std::memory_order_acquire);
    }

    // Logic thread. True exactly once after the streaming thread has failed, so
    // the caller emits one log record for it. A dead streaming subsystem must
    // not be silent: without a record the integrator only sees sound stop.
    bool ConsumeWorkerFailureForLog() noexcept;

    // Logic thread, after the streaming thread has failed: returns slots the
    // audio thread has retired to Free. That reclamation is normally the
    // streaming thread's job, so without this a failure would drain the stream
    // budget permanently. A no-op unless the worker has failed.
    void ReclaimAfterWorkerFailure();

private:
    struct StreamFile {
        VnSoundId soundId{};
        // A replaced-away old generation: no longer registered under its id
        // (FindFile, register-duplicate, and unregister all skip it), kept
        // open only for the slots and pending pins still reading it by file
        // index. retiredFromId keys the one-retiring-generation-per-asset
        // refusal. The streaming thread closes the file once both counts
        // drain.
        bool retiring = false;
        VnSoundId retiredFromId{};
        std::ifstream file; // streaming thread only
        StreamTocEntry toc;
        std::uint32_t chunkFrames = 0;
        std::vector<std::uint64_t> chunkChecksums;
        std::uint64_t contentRevision = 0;
        // Slots currently bound to this file: incremented by the logic
        // thread at activate, decremented by the streaming thread at
        // detach; unregister refuses while non-zero.
        std::atomic<std::uint32_t> activeStreams{0};
        // Steal pending-start voices holding this file while they await a
        // render slot: incremented and decremented by the logic thread (under
        // the mutex); unregister refuses while non-zero so the source survives
        // the pending window and cannot be swapped.
        std::atomic<std::uint32_t> pendingRefs{0};
    };

    static constexpr std::uint32_t kNoChunk = 0xffffffffu;

    struct StreamSlot {
        StreamRing ring;
        // Written by the logic thread before the Active publication;
        // read by the streaming thread after it (release/acquire on
        // ring.state orders the two).
        std::uint32_t fileIndex = 0;
        // Streaming-thread-only serve state.
        std::uint64_t nextFrame = 0;     // next source frame to push
        std::uint64_t loopStartFrame = 0;
        std::uint64_t loopEndFrame = 0;
        bool loopInfinite = false;
        std::uint32_t loopPassesRemaining = 1;
        std::uint32_t stagedChunk = kNoChunk;
        bool feedStopped = false;        // end reached or integrity abort
        std::vector<float> staging;      // one verified chunk, planar
    };

    struct Request {
        enum class Kind { Register, Unregister, Replace, Serve } kind;
        VnSoundId soundId{};
        const char* utf8Path = nullptr; // valid: requester blocks until done
        VnResult result = VN_OK;
        bool done = false;
    };

    void ThreadMain();
    VnResult PostAndWait(Request& request);
    void HandleRegister(Request& request);
    void HandleUnregister(Request& request);
    void HandleReplace(Request& request);
    // Opens and validates one .stream file into `record` (soundId already
    // set): the whole RegisterFile validation chain, shared with replace.
    // Streaming thread only.
    VnResult OpenAndValidate(Request& request, StreamFile& record);
    std::uint32_t OccupiedSlotPrefix() const noexcept;
    bool PrepareRingStorage(std::uint32_t slotPrefix, std::uint32_t channels,
                            bool pending = false) noexcept;
    bool RingStorageReady(std::uint32_t slot, std::uint32_t channels) const noexcept;
    // Publishes a lifecycle transition and wakes both the constructor's
    // handshake and any request waiter, whose predicates both consult it.
    void PublishWorkerState(StreamWorkerState state) noexcept;
    // Completes every queued request with VN_DEVICE_ERROR. Caller holds the
    // mutex. Used when the streaming thread stops for any reason: a request
    // left un-completed is a caller blocked forever.
    void FailQueuedRequestsLocked() noexcept;
    // Publishes end-of-data on every slot the streaming thread was feeding, so
    // the audio thread drains what is buffered and ends those voices through
    // the path it already runs. Without this a stream voice whose producer died
    // stays Active and silent forever and never returns its slot. Lock-free:
    // one release store per slot into the ring the audio thread already reads.
    void EndActiveStreamsOnFailure() noexcept;
    // The streaming thread's terminal path: publish Failed, release every
    // waiter, and close out the rings. Must not throw -- it runs from the
    // thread-entry catch, where there is nowhere left to report.
    void HandleTerminalWorkerFailure() noexcept;
    void DetachRetiredSlots();
    // Closes retiring files whose active and pending counts both drained.
    // Streaming thread, under the mutex (m_files mutation).
    void CloseDrainedRetiringFiles();
    void ServeSlots();
    void ServeOneSlot(StreamSlot& slot);

    // One unit of streaming work: read a named chunk of a named file and prove
    // its integrity. Deciding WHICH chunk comes next is slot scheduling -- read
    // cursor, loop traversal, ring occupancy -- and stays with the streaming
    // thread that owns that state. This half touches only the file and the CPU
    // and reads no slot state at all, which is the seam: an asynchronous I/O
    // stage or a decoding job pool can take this over, submitting jobs and
    // collecting staged buffers, without the cursor and loop bookkeeping moving
    // with it.
    struct ChunkJob {
        StreamFile* file = nullptr;
        std::uint32_t chunkIndex = 0;
        std::uint32_t chunkFrames = 0;
        std::uint32_t channels = 0;
        std::uint64_t chunkBytes = 0;
    };
    // Stages one whole verified chunk into `staging`. False on a short read or a
    // checksum mismatch; the caller decides what that means for the slot, since
    // only the caller knows the slot. Static: the job depends on nothing but its
    // arguments, which is what makes it relocatable.
    static bool RunChunkJob(const ChunkJob& job, std::vector<float>& staging);

    std::uint32_t m_ringFrames = 0; // per-slot capacity, power of two

    // Registered files. Mutated only by the streaming thread (under the
    // mutex); the logic thread reads under the mutex (FindFile). Stable
    // addresses so slots can hold indices across table growth.
    std::vector<std::unique_ptr<StreamFile>> m_files;

    std::vector<StreamSlot> m_slots;
    friend struct detail::StreamManagerTestAccess;
    std::array<StableRowStorage<float>, kMaxChannels> m_ringStorage;
    // Logic-owned backing demand follows the existing file pending references.
    std::array<std::uint32_t, kMaxChannels> m_pendingRingSlots{};

    // Request transport between the logic and streaming threads. The
    // audio thread never touches any of this.
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;  // streaming thread waits here
    std::condition_variable m_done;  // requesters wait here
    std::deque<Request*> m_requests;
    bool m_shutdown = false;

    // Reusable per-iteration request batch. A member rather than a streaming-
    // thread local so that reaching Ready involves no allocation at all: the
    // only startup allocation used to live on that thread, where a bad_alloc
    // killed it silently. Here the same failure is a constructor throw, which
    // Vn_Init already turns into a defined result code.
    std::deque<Request*> m_batch;

    std::atomic<StreamWorkerState> m_workerState{StreamWorkerState::Starting};
    // Signals lifecycle transitions to the constructor's startup handshake.
    std::condition_variable m_stateChanged;
    // Logic thread only: whether the failure has been reported to the log sink,
    // so one record is emitted rather than one per Update.
    bool m_workerFailureLogged = false;

    std::atomic<bool> m_throttled{false};
    std::atomic<std::uint32_t> m_maxServeFramesForTest{0};
    std::atomic<std::uint64_t> m_testActivationStartFrame{0};
    std::atomic<bool> m_testActivationStartFrameSet{false};

    // Most slots ever Active at once. Claimed on the logic thread only,
    // so the high-water is logic-owned plain state; the live count is
    // atomic because the streaming thread decrements at detach.
    std::atomic<std::uint32_t> m_activeSlots{0};
    std::uint64_t m_streamHighWater = 0;

    // Fullest any ring has been, in frames; producer-updated (occupancy
    // peaks right after a write, so the producer-side max is exact).
    std::atomic<std::uint64_t> m_ringHighWaterFrames{0};

    std::thread m_thread; // last: joins in the destructor
};

} // namespace harken

#endif // HARKEN_MEDIA_STREAM_MANAGER_H
