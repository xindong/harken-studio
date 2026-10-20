// Streaming-thread lifecycle: the startup handshake, and what happens to
// everyone waiting on the streaming thread when it dies.
//
// The logic thread reaches the streaming thread by posting a request and
// blocking until it is served, so a streaming thread that dies without
// completing the outstanding requests strands its callers permanently. These
// tests therefore never call a stream API directly on the test thread: each
// blocking call runs on a detached thread and is awaited with a bounded wait,
// so a regression reports as a failed expectation instead of a hung suite.
//
// A test that drives the streaming thread into its terminal path leaves that
// detached thread blocked; the expectations below are written so the bounded
// wait is what fails, and the process is expected to be terminated externally
// rather than to unwind through engine state a stranded thread still touches.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"

#include "harken/media/StreamFormat.h"
#include "harken/media/StreamManager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::BuildStreamBytes;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::StreamSpec;

// Generous enough that a healthy streaming thread (2 ms service interval)
// always wins, short enough that a stranded caller is reported quickly.
constexpr std::chrono::milliseconds kCallBudget{4000};

struct StreamRequestFaultGuard {
    explicit StreamRequestFaultGuard(harken::StreamRequestFault fault) {
        harken::SetStreamRequestFaultForTests(fault);
    }
    ~StreamRequestFaultGuard() {
        harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::None);
    }
    StreamRequestFaultGuard(const StreamRequestFaultGuard&) = delete;
    StreamRequestFaultGuard& operator=(const StreamRequestFaultGuard&) = delete;
};

// Request completion can precede the terminal worker-state publication.
bool WaitForWorkerFailure(const harken::StreamManager& streams) {
    const auto deadline = std::chrono::steady_clock::now() + kCallBudget;
    while (streams.WorkerState() != harken::StreamWorkerState::Failed &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return streams.WorkerState() == harken::StreamWorkerState::Failed;
}

// One blocking stream call, run off the test thread so the test survives a
// call that never returns. Shared state outlives the detached thread on
// purpose: a stranded thread must not write into a destroyed frame.
struct BoundedCall {
    struct Shared {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        VnResult result = VN_OK;
    };

    std::shared_ptr<Shared> state = std::make_shared<Shared>();

    template <typename Fn>
    void Start(Fn call) {
        std::shared_ptr<Shared> shared = state;
        std::thread([shared, call]() {
            const VnResult result = call();
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->result = result;
                shared->finished = true;
            }
            shared->done.notify_all();
        }).detach();
    }

    // True when the call returned inside the budget; `out` then holds it.
    bool Wait(VnResult* out) const {
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool finished =
            state->done.wait_for(lock, kCallBudget,
                                 [this] { return state->finished; });
        if (finished && out != nullptr) {
            *out = state->result;
        }
        return finished;
    }
};

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

// A minimal valid single-channel stream file on disk.
std::string WriteStream(const std::string& name, VnSoundId soundId) {
    StreamSpec spec;
    spec.soundId = soundId;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    const std::vector<float> planar(2048, 0.25f);
    const std::vector<std::uint8_t> bytes = BuildStreamBytes(spec, planar);
    const std::string path = TempPath(name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

// The handshake succeeds: a healthy streaming thread reports Ready and
// serves a registration.
TEST(StreamWorkerLifecycle, HandshakePublishesReadyAndServesRequests) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::StreamWorkerState::Ready,
              session.Engine()->streams.WorkerState());

    const std::string path = WriteStream("swl_ready.stream", harkentest::ObjectId(9101));
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(9101), path.c_str()));
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(9101)));
}

// The streaming thread cannot start: initialization must fail outright
// rather than hand back an engine whose every stream call would strand its
// caller.
TEST(StreamWorkerLifecycle, StartupFailureFailsInitInsteadOfStranding) {
    StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerStartup);

    BoundedCall init;
    init.Start([] {
        const VnConfig config = MakeNullConfig();
        const VnResult result = Vn_Init(&config);
        if (result == VN_OK) {
            Vn_Term();
        }
        return result;
    });

    VnResult result = VN_OK;
    ASSERT_TRUE(init.Wait(&result))
        << "Vn_Init did not return within the budget: the streaming thread "
           "died at startup and initialization waited on it forever";
    EXPECT_EQ(VN_INVALID_CONFIG, result)
        << "a streaming thread that cannot start must fail initialization, "
           "not produce an engine whose stream calls strand the caller";
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
}

// The streaming thread dies while serving: the in-flight caller and every
// later caller must be released with a defined result instead of blocking.
TEST(StreamWorkerLifecycle, WorkerDeathReleasesWaitersInsteadOfBlocking) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string path = WriteStream("swl_death.stream", harkentest::ObjectId(9102));

    StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerLoop);

    // The fault fires on the streaming thread's next loop iteration, so this
    // call may legitimately still be served before the thread dies. What must
    // never happen is that it does not come back at all.
    BoundedCall first;
    const std::string firstPath = path;
    first.Start([firstPath] {
        return Vn_RegisterStreamFile(harkentest::ObjectId(9102), firstPath.c_str());
    });

    VnResult firstResult = VN_OK;
    ASSERT_TRUE(first.Wait(&firstResult))
        << "the in-flight registration never returned: the streaming thread "
           "died and left the caller waiting on a completion nobody will post";
    EXPECT_TRUE(firstResult == VN_OK || firstResult == VN_DEVICE_ERROR)
        << "served or refused are both fine; blocked is not. got " << firstResult;

    // A caller arriving after the death must be refused immediately, without
    // being queued behind a thread that will never run again.
    ASSERT_TRUE(WaitForWorkerFailure(session.Engine()->streams))
        << "the streaming thread never published Failed within the budget";
    BoundedCall later;
    later.Start([firstPath] {
        return Vn_RegisterStreamFile(harkentest::ObjectId(9103), firstPath.c_str());
    });
    VnResult laterResult = VN_OK;
    ASSERT_TRUE(later.Wait(&laterResult))
        << "a stream call issued after the streaming thread died never "
           "returned";
    EXPECT_EQ(VN_DEVICE_ERROR, laterResult);

    EXPECT_EQ(harken::StreamWorkerState::Failed,
              session.Engine()->streams.WorkerState());

    // A call that unwinds early because the thread died must not leave its
    // Request queued: that object lives on the caller's stack, so a stale
    // pointer here is dereferenced by whoever drains the queue next.
    EXPECT_EQ(0u, session.Engine()->streams.PendingRequestCountForTest())
        << "a request that returned early is still queued, so the queue holds a "
           "pointer into a destroyed stack frame";
}

// Starting a streamed voice after the thread died strands both slots it claims:
// nothing fills the ring so the prebuffer gate never opens, and nothing marks the
// source ended so the voice neither starves nor finishes. It would hold a stream
// slot and a voice slot for the rest of the session, reporting PREPARING forever,
// with no notification and no starvation count to show for it.
TEST(StreamWorkerLifecycle, PlayAfterWorkerDeathIsRefusedAndClaimsNoSlot) {
    VnConfig config = MakeNullConfig(4, 256, 48000);
    config.maxStreams = 1;
    config.streamRingFrames = 1024;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = WriteStream("swl_play_after_death.stream", harkentest::ObjectId(9107));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(9107), path.c_str()));

    const std::uint32_t freeSlotsBefore =
        session.Engine()->streams.FreeSlotCount();
    ASSERT_GT(freeSlotsBefore, 0u);

    {
        StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerLoop);
        BoundedCall sync;
        sync.Start([&session] {
            session.Engine()->streams.SyncServe();
            return VN_OK;
        });
        VnResult ignored = VN_OK;
        ASSERT_TRUE(sync.Wait(&ignored));
        ASSERT_TRUE(WaitForWorkerFailure(session.Engine()->streams))
            << "the streaming thread never published Failed within the budget";
        ASSERT_EQ(harken::StreamWorkerState::Failed,
                  session.Engine()->streams.WorkerState());

        // The id is still registered, so resolution succeeds; only liveness
        // refuses it.
        const VnPlayDesc desc = harkentest::MakePlayDesc(harkentest::ObjectId(9107));
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_DEVICE_ERROR, harkentest::PlayRaw(&desc, &handle))
            << "a streamed play was admitted after the streaming thread died; "
               "nothing will ever feed or end that voice";

        EXPECT_EQ(freeSlotsBefore, session.Engine()->streams.FreeSlotCount())
            << "the refused play still consumed a stream slot";

        // And no voice slot was taken either.
        Vn_Update();
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(VnSnapshot);
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_EQ(0u, snapshot.activeVoiceCount)
            << "the refused play left a voice slot occupied";
    }
}

// A voice already streaming when the thread dies must end deterministically
// instead of staying Active and silent forever, which would also never return
// its stream slot.
TEST(StreamWorkerLifecycle, WorkerDeathEndsActiveStreamVoices) {
    VnConfig config = MakeNullConfig(4, 256, 48000);
    config.maxStreams = 1;
    config.streamRingFrames = 1024;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = WriteStream("swl_active.stream", harkentest::ObjectId(9104));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(9104), path.c_str()));
    harken::StreamManager::StreamFacts probe;
    ASSERT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(9104), &probe))
        << "registration reported success but the id does not resolve";

    const VnPlayDesc desc = harkentest::MakePlayDesc(harkentest::ObjectId(9104));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&desc, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint32_t freeBefore = session.Engine()->streams.FreeSlotCount();

    {
        StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerLoop);
        // Give the streaming thread its next service wake to die in.
        BoundedCall sync;
        sync.Start([&session] {
            session.Engine()->streams.SyncServe();
            return VN_OK;
        });
        VnResult ignored = VN_OK;
        ASSERT_TRUE(sync.Wait(&ignored))
            << "SyncServe never returned after the streaming thread died";

        ASSERT_TRUE(WaitForWorkerFailure(session.Engine()->streams))
            << "the streaming thread never published Failed within the budget";
        EXPECT_EQ(harken::StreamWorkerState::Failed,
                  session.Engine()->streams.WorkerState());

        // Render past the buffered remainder: end-of-data lets the voice finish
        // on the audio thread, which retires the slot; Vn_Update is what returns
        // it to Free now that the streaming thread is gone.
        for (int block = 0; block < 64; ++block) {
            session.Engine()->nullBackend.Pump(1);
            Vn_Update();
        }
    }

    EXPECT_GT(session.Engine()->streams.FreeSlotCount(), freeBefore)
        << "the stream slot was never returned: the voice is still Active and "
           "silent, so its slot leaks for the rest of the session";
}

// Shutdown while a request is still queued. Breaking out of the service loop
// happens before the swap that claims the queue, so a request sitting there was
// dropped un-completed and its caller waited forever for a completion nobody
// would post.
//
// This drives StreamManager directly rather than the public API: that API is
// externally synchronized, so a caller cannot be mid-request while another thread
// tears the engine down, and the path would otherwise have no coverage at all.
// The stall hook opens a deterministic window instead of racing for one.
TEST(StreamWorkerLifecycle, ShutdownCompletesRequestsStillQueued) {
    harken::EngineConfig config{};
    config.maxStreams = 2;
    config.streamRingFrames = 1024;

    const std::string path = WriteStream("swl_shutdown.stream", harkentest::ObjectId(9105));

    harken::SetStreamWorkerStallReleaseForTests(false);
    const std::uint32_t stallsBefore = harken::StreamWorkerStallEntriesForTests();

    BoundedCall queued;
    {
        auto streams = std::make_unique<harken::StreamManager>(config);
        ASSERT_EQ(harken::StreamWorkerState::Ready, streams->WorkerState());

        StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerStall);

        // First request: the streaming thread claims it, then stalls with the
        // queue unlocked.
        harken::StreamManager* raw = streams.get();
        BoundedCall stalling;
        const std::string p = path;
        stalling.Start([raw, p] { return raw->RegisterFile(harkentest::ObjectId(9105), p.c_str()); });

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (harken::StreamWorkerStallEntriesForTests() == stallsBefore &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_GT(harken::StreamWorkerStallEntriesForTests(), stallsBefore)
            << "the streaming thread never reached the stall window";

        // Second request queues behind the stalled pass and is not claimed.
        queued.Start([raw, p] { return raw->RegisterFile(harkentest::ObjectId(9106), p.c_str()); });
        const auto queuedBy =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (raw->PendingRequestCountForTest() == 0 &&
               std::chrono::steady_clock::now() < queuedBy) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_GT(raw->PendingRequestCountForTest(), 0u)
            << "the second request never reached the queue";

        // Release the stall from another thread: the destructor below sets
        // shutdown and then joins, so the streaming thread must be free to run.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            harken::SetStreamWorkerStallReleaseForTests(true);
        }).detach();

        streams.reset(); // shutdown + join, with a request still queued
    }
    harken::SetStreamWorkerStallReleaseForTests(false);

    VnResult queuedResult = VN_OK;
    ASSERT_TRUE(queued.Wait(&queuedResult))
        << "a request queued when shutdown began never returned: it was dropped "
           "un-completed and its caller waits for a completion nobody will post";
    EXPECT_EQ(VN_DEVICE_ERROR, queuedResult);
}

// Defence in depth behind the liveness pre-check: a play that passed the check
// microseconds before the state flipped publishes an Active slot after the
// terminal path has already swept the rings, so nothing will ever feed or end it.
//
// The slot must therefore be activated AFTER the failure to exercise this at all.
// Activating before it is closed out by the terminal sweep instead, which is why
// this drives StreamManager directly: the public play path now refuses once the
// thread is gone, so there is no way through it to reach this state, and going
// through it would only re-test the terminal sweep.
TEST(StreamWorkerLifecycle, ReclamationClosesASlotActivatedAfterTheFailure) {
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingFrames = 1024;

    const std::string path = WriteStream("swl_race_slot.stream", harkentest::ObjectId(9108));

    harken::StreamManager streams(config);
    ASSERT_EQ(harken::StreamWorkerState::Ready, streams.WorkerState());
    ASSERT_EQ(VN_OK, streams.RegisterFile(harkentest::ObjectId(9108), path.c_str()));
    harken::StreamManager::StreamFacts facts;
    ASSERT_TRUE(streams.FindFile(harkentest::ObjectId(9108), &facts));
    ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, facts.channelCount));

    {
        StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerLoop);
        BoundedCall sync;
        harken::StreamManager* raw = &streams;
        sync.Start([raw] { raw->SyncServe(); return VN_OK; });
        VnResult ignored = VN_OK;
        ASSERT_TRUE(sync.Wait(&ignored));
        ASSERT_TRUE(WaitForWorkerFailure(streams))
            << "the streaming thread never published Failed within the budget";
        ASSERT_EQ(harken::StreamWorkerState::Failed, streams.WorkerState());

        // The slipped-through activation: published after the terminal sweep ran.
        std::uint32_t slot = 0;
        ASSERT_TRUE(streams.PeekFreeSlot(&slot));
        streams.ActivateSlot(slot, facts.fileIndex, 0, 0, 0, 1);
        harken::StreamRing* ring = streams.RingOf(slot);
        ASSERT_NE(nullptr, ring);
        ASSERT_EQ(harken::StreamSlotState::Active,
                  ring->state.load(std::memory_order_acquire));
        ASSERT_EQ(harken::StreamTerminalReason::None, ring->TerminalReason())
            << "precondition: this slot is open and unfed";

        streams.ReclaimAfterWorkerFailure();

        EXPECT_EQ(harken::StreamTerminalReason::ProducerFailure,
                  ring->TerminalReason())
            << "a slot activated after the failure was left open, so the voice on "
               "it never ends and its slot is held for the rest of the session";
    }
}

// Retiring files are closed only by the streaming thread, so after it dies a
// replaced-away generation would stay open and keep counting for the session.
TEST(StreamWorkerLifecycle, WorkerDeathStillClosesDrainedRetiringFiles) {
    VnConfig config = MakeNullConfig(4, 256, 48000);
    config.maxStreams = 2;
    config.streamRingFrames = 1024;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string first = WriteStream("swl_retire_a.stream", harkentest::ObjectId(9109));
    const std::string second = WriteStream("swl_retire_b.stream", harkentest::ObjectId(9109));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(9109), first.c_str()));

    // Play, then replace while it plays: the old generation retires with a
    // consumer still on it.
    const VnPlayDesc desc = harkentest::MakePlayDesc(harkentest::ObjectId(9109));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&desc, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_ReplaceStreamFile(harkentest::ObjectId(9109), second.c_str()));
    ASSERT_EQ(1u, session.Engine()->streams.RetiringFileCount());

    {
        StreamRequestFaultGuard fault(harken::StreamRequestFault::WorkerLoop);
        BoundedCall sync;
        sync.Start([&session] {
            session.Engine()->streams.SyncServe();
            return VN_OK;
        });
        VnResult ignored = VN_OK;
        ASSERT_TRUE(sync.Wait(&ignored));
        ASSERT_TRUE(WaitForWorkerFailure(session.Engine()->streams))
            << "the streaming thread never published Failed within the budget";
        ASSERT_EQ(harken::StreamWorkerState::Failed,
                  session.Engine()->streams.WorkerState());

        for (int block = 0; block < 64; ++block) {
            session.Engine()->nullBackend.Pump(1);
            Vn_Update();
        }
    }

    EXPECT_EQ(0u, session.Engine()->streams.RetiringFileCount())
        << "the retiring generation was never closed, so its handle leaks and "
           "the retiring count stays permanently high";
}

} // namespace
