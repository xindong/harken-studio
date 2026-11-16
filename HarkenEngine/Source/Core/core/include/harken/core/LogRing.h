// Audio-side log ring: fixed-size POD records pushed wait-free on the
// audio thread; formatting to text happens only when the logic thread
// drains the ring inside Vn_Update. A full ring overwrites the oldest
// unread record (drop-oldest) and the consumer counts the overwritten
// span at drain time — the notification queue's overwrite realization on the
// producer side, with an added consumer revalidation step that makes
// the drain safe against a producer lapping the ring mid-copy.
//
// Storage is inline in the ring object (the engine owns it); push and pop
// touch no heap, take no locks, and format no text.
#ifndef HARKEN_CORE_LOG_RING_H
#define HARKEN_CORE_LOG_RING_H

#include <harken/ObjectId.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace harken {

inline std::uint64_t LogTimestampNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Condition codes carried in LogRecord.code. Engine-internal vocabulary
// for now; a public code registry becomes an API surface only when hosts
// need to interpret records structurally rather than reading the text.
inline constexpr std::uint32_t kLogCodeArenaOverflow = 1;
// Action outcomes (logic thread): a registration rejected by validation
// (arg0 = Action id), a triggered Step skipped by a runtime failure
// (arg0 = Action id, arg1 = Step index), and a delayed Step dropped
// because its triggering emitter died before it was serviced (arg0 = Action id,
// arg1 = Step index).
inline constexpr std::uint32_t kLogCodeActionRegisterRejected = 4;
inline constexpr std::uint32_t kLogCodeActionStepSkipped = 5;
inline constexpr std::uint32_t kLogCodeActionStepDroppedDeadEmitter = 6;
// Audibility parking (logic thread): a park deferred because the voice's most
// recent source cursor was stale/unavailable this tick (arg0 = public handle;
// also counted in parkDeferredCount), and a park skipped because the command
// queue had no room for the internal stop this tick (arg0 = public handle;
// logged, retried, deliberately NOT counted as a park deferral).
inline constexpr std::uint32_t kLogCodeParkDeferredStaleCursor = 7;
inline constexpr std::uint32_t kLogCodeParkStopDeferredNoCommandRoom = 8;
// Trigger-graph outcomes (logic thread): one record per edge of a refused
// trigger cycle's closed walk (arg0 = from Action id, arg1 = to Action id;
// the drained record sequence names the exact refused chain), and a nested
// trigger firing dropped by the depth ceiling (arg0 = the chain's outermost
// Action id, arg1 = the refused target Action id).
inline constexpr std::uint32_t kLogCodeActionTriggerCycle = 9;
inline constexpr std::uint32_t kLogCodeActionTriggerDepthDropped = 10;
// Hot-reload outcomes (logic thread): every replace verb records its result —
// applied or refused — with arg0 = the asset class (kHotReloadClass*) and
// arg1 = the asset id (or the raw pack handle for the pack class), so tooling
// can observe content swaps through the ordinary log sink.
inline constexpr std::uint32_t kLogCodeHotReloadApplied = 11;
inline constexpr std::uint32_t kLogCodeHotReloadRefused = 12;
// Session-trace failure (logic thread): a mid-session trace write failed, so
// the trace stopped while the engine kept running (arg0 = the journal's
// record ordinal at the failure). The snapshot's sticky traceFailed flag is
// the machine-readable twin of this record.
inline constexpr std::uint32_t kLogCodeTraceWriteFailed = 13;
// Property publication capacity contract (logic thread): a wiring growth
// was refused because the worst-case one-tick publication would exceed the
// command queue's capacity (arg0 = proposed worst-case commit count,
// arg1 = queue capacity). Structural refusal, cleared by detaching or
// configuring a larger queue.
inline constexpr std::uint32_t kLogCodePropertyCapacityRefused = 14;
// Property publication batch overflow (logic thread): one tick's dirty
// set exceeded the fixed batch buffer (arg0 = commits already staged,
// arg1 = batch capacity). Reachable when spatializer-owned contributions
// — which sit outside the binding-instance bound — dirty more targets
// than the command queue holds; publication degrades to capacity-sized
// chunks across successive ticks and converges once traffic settles.
inline constexpr std::uint32_t kLogCodePropertyBatchOverflow = 15;
// Config-time worst-case estimate (logic thread, emitted once at init):
// every voice's spatially drivable targets plus every bus target row
// exceed the command queue's capacity (arg0 = estimate, arg1 = queue
// capacity), so publication will chunk under full simultaneous load.
// Observability only — nothing is refused.
inline constexpr std::uint32_t kLogCodePropertyCapacityEstimate = 16;
// Binding pool exhaustion (logic thread): a fixed binding pool refused a
// growth (arg0 = which pool, one of kBindingPool* below; arg1 = that
// pool's capacity). The no-binding refusal's log record names which pool
// so the config remedy is unambiguous. Also emitted when a play is
// refused at acceptance because its matched attachment set exceeds the
// per-voice instance slots (the instance pool row, arg1 = the ceiling).
inline constexpr std::uint32_t kLogCodeBindingPoolExhausted = 17;
// Meter control-loop freeze (logic thread): a LATER topology change
// closed a meter/binding/bus
// control loop, so the offending binding's contribution froze at neutral
// (arg0 = the meter bus id whose Variable drives the loop, arg1 = the
// attached bus id whose volume it drives). One record per freeze
// transition; meterLoopCount is the machine-readable twin. Wiring-time
// loops are refused VN_METER_CYCLE and never reach here.
inline constexpr std::uint32_t kLogCodeMeterLoopFrozen = 18;
// Meter ring overflow (logic thread, drained accounting): blocks were
// max-hold compressed because the meter transport ring filled between
// pumps (arg0 = blocks compressed since the previous drain, arg1 = the
// cumulative total). Peaks are never lost, at worst time-compressed; the
// remedy is a deeper meterRingBlocks or a steadier pump cadence.
inline constexpr std::uint32_t kLogCodeMeterRingOverflow = 19;
// Capture truncation (logic thread): a capture call's
// caller-provided buffer was smaller than
// the row set (arg0 = which capture surface, one of kCaptureSurface*
// below; arg1 = the required row count). The call still succeeded with
// the truncated prefix written and *requiredCount reporting the full
// need; the record is the flagged trail of that truncation.
inline constexpr std::uint32_t kLogCodeCaptureTruncated = 20;
// Pack logic-entry refusals (logic thread). Closure: a v2 pack's logic
// references failed to close
// against {itself + already-loaded packs} (arg0 = the missing id,
// arg1 = the reference class, a PackClosureRef value). Resident unload:
// Vn_UnloadPack refused on a resident pack — engine-lifetime
// infrastructure; Vn_ReplacePack is the dev-iteration path (arg0 = the
// pack handle). Provider protection: unloading or replacing a provider
// would orphan a loaded consumer (arg0 = the provider pack handle,
// arg1 = the consumer pack handle — the consumer named in the log).
inline constexpr std::uint32_t kLogCodePackClosureRefused = 21;
inline constexpr std::uint32_t kLogCodePackResidentUnloadRefused = 22;
inline constexpr std::uint32_t kLogCodePackProviderInUse = 23;
// A pack apply/unwind seam returned a refusal AFTER staging proved every
// refusal cause absent (arg0 = the failing entry's id, arg1 = the result
// code as an unsigned value). Structurally unreachable; a nonzero count
// of these records marks a staging bookkeeping defect, never a content
// error.
inline constexpr std::uint32_t kLogCodePackApplyDefect = 24;
// A staged pack revision was discarded at its activation tick because
// the world drifted during the gap between the replace call returning
// and activation (an id the revision needs was taken, a removed entry
// gained references, a pool filled). The old revision stays fully in
// force (arg0 = the pack handle, arg1 = the re-screen's result code as
// an unsigned value).
inline constexpr std::uint32_t kLogCodePackRevisionDiscarded = 25;
// Action PLAY at a live emitter: a resolved multichannel leaf was demoted to
// 2D while retaining its emitter attachment (arg0 = Action id, arg1 = Step
// index).
inline constexpr std::uint32_t kLogCodeActionEmitterMultichannelFallback = 26;
// Logic-thread allocation degradation (arg0 = private LogicAllocSite,
// arg1 = affected public handle or object id, when available).
inline constexpr std::uint32_t kLogCodeLogicAllocFailure = 27;
// Streaming thread died (logic thread, emitted once when the failure is first
// observed). Every stream call is refused from then on and the voices it was
// feeding end, so this record is the only positive evidence distinguishing a
// dead streaming subsystem from content that simply stopped.
inline constexpr std::uint32_t kLogCodeStreamWorkerFailed = 28;
// Control commands have been arriving faster than a quantum applies them for
// long enough that the backlog is not a momentary burst (audio thread; arg0 =
// records left over on the block that tripped it, arg1 = consecutive blocks
// behind). Emitted once per episode: a run that recovers and then falls behind
// again reports again, because a once-per-process signal would go quiet for the
// rest of a long session after the first burst.
inline constexpr std::uint32_t kLogCodeCommandBudgetBehind = 29;
// Render-worker synchronization exceeded its wait budget (audio thread;
// arg0 = microseconds past that budget, arg1 = jobs not observed finished).
// The prepared quantum and its voices are retained until payload completion.
// Later callbacks poll without advancing playback time or reclaiming storage.
inline constexpr std::uint32_t kLogCodeRenderBatchDeferred = 30;
// The worker cutoff is below the scheduler jitter warning threshold (logic
// thread, once at init; arg0 = cutoff offset in microseconds, arg1 = warning
// threshold). Short blocks retain their own bound; the warning never raises it.
inline constexpr std::uint32_t kLogCodeRenderDeadlineBelowJitterFloor = 31;
inline constexpr std::uint32_t kLogCodePackContentConflict = 32;
// Selection backing was unavailable (logic thread). arg0 packs failure kind
// in the low byte and resource in the next byte; arg1 is the requested count.
inline constexpr std::uint32_t kLogCodeSelectionStorageRefused = 33;

// Binding pools named by kLogCodeBindingPoolExhausted's arg0. Log
// vocabulary only, never wire or ABI state.
inline constexpr std::uint64_t kBindingPoolDefinitions = 0;
inline constexpr std::uint64_t kBindingPoolAttachments = 1;
inline constexpr std::uint64_t kBindingPoolVoiceInstances = 2;

// Capture surfaces named by kLogCodeCaptureTruncated's arg0. Log
// vocabulary only, never wire or ABI state.
inline constexpr std::uint64_t kCaptureSurfaceTargets = 0;
inline constexpr std::uint64_t kCaptureSurfaceBindings = 1;
inline constexpr std::uint64_t kCaptureSurfaceEffectCapabilities = 2;
inline constexpr std::uint64_t kCaptureSurfaceEffectParameterCapabilities = 3;
inline constexpr std::uint64_t kCaptureSurfaceEffectDynamicCapabilities = 4;
inline constexpr std::uint64_t
    kCaptureSurfaceEffectDynamicParameterCapabilities = 5;
inline constexpr std::uint64_t kCaptureSurfaceEffectDynamicRoutes = 6;
inline constexpr std::uint64_t kCaptureSurfaceCompressorRuntime = 7;

// Asset classes named by the hot-reload log records' arg0. Log vocabulary
// only, never wire or ABI state.
inline constexpr std::uint64_t kHotReloadClassPCM = 0;
inline constexpr std::uint64_t kHotReloadClassStream = 1;
inline constexpr std::uint64_t kHotReloadClassPack = 2;
inline constexpr std::uint64_t kHotReloadClassAttenuation = 3;
inline constexpr std::uint64_t kHotReloadClassAction = 4;
inline constexpr std::uint64_t kHotReloadClassContainer = 5;
inline constexpr std::uint64_t kHotReloadClassBinding = 6;

// One log record: severity (VN_LOG_*), condition code, and two raw
// integer arguments whose meaning the code defines. Binary only — the
// audio thread never builds strings.
struct LogRecord {
    std::uint32_t level;
    std::uint32_t code;
    std::uint64_t arg0;
    std::uint64_t arg1;
    std::uint64_t timestampNs = 0;
    VnObjectId objectId{};
    VnObjectId projectId{};
};

static_assert(std::is_trivially_copyable_v<LogRecord>,
              "log records cross threads by memcpy semantics");
static_assert(sizeof(LogRecord) == 64, "log record layout is frozen");
static_assert(sizeof(LogRecord) == sizeof(std::uint64_t[8]),
              "the record maps onto exactly eight slot words");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "slot words must be lock-free for a wait-free audio-thread push");

class LogRing {
public:
    // Power-of-two capacity so index masking replaces modulo. Fixed at
    // compile time: the ring is a bounded diagnostics channel, not a
    // load-bearing transport, so it is not a tuning parameter.
    static constexpr std::uint32_t kCapacity = 256;

    // Opts this ring into the per-quantum producer budget. Off by default,
    // because only the audio-thread ring has quanta: the logic-side ring is
    // pushed from ordinary API calls that belong to no block, and a budget
    // there would refuse records for being outside a quantum that does not
    // exist. The audio ring's owner turns this on at construction.
    void EnableQuantumBudget() { m_quantumBudgetEnabled = true; }

    // Opens a quantum: clears what this block has produced so far. Only the
    // per-quantum count resets -- the budget-drop total accumulates across the
    // session and the produced high-water stays monotone, because both exist to
    // be read after the fact.
    void BeginQuantum() { m_producedThisQuantum = 0; }

    // Producer side (audio thread). When the ring is full the oldest unread
    // record is overwritten, as before.
    //
    // Returns false only when the per-quantum budget refused the record. The
    // budget equals the ring's capacity: a block that produces more records
    // than the ring can hold has already made the earlier ones unreachable, so
    // the ones past capacity buy nothing and cost audio-thread time in a path
    // whose cost is meant to be bounded. Refusal is a drop, never a defer --
    // there is no later block a log record could belong to.
    //
    // Existing callers ignore the result and must keep ignoring it: whether a
    // diagnostic record was kept may not change what the block renders. The
    // return exists for tests and accounting.
    bool Push(const LogRecord& record) {
        if (m_quantumBudgetEnabled) {
            if (m_producedThisQuantum >= kCapacity) {
                ++m_quantumBudgetDropped;
                return false;
            }
            ++m_producedThisQuantum;
            if (m_producedThisQuantum > m_producedHighWater) {
                m_producedHighWater = m_producedThisQuantum;
            }
        }
        PushUnbudgeted(record);
        return true;
    }

    // Records this ring refused because a single quantum had already produced
    // its capacity. Distinct from DroppedCount(), which counts records the
    // producer overwrote before the consumer read them: one says a block
    // produced too much, the other says the consumer drained too late, and a
    // single number could not tell a caller which to fix.
    std::uint64_t QuantumBudgetDropCount() const {
        return m_quantumBudgetDropped.load(std::memory_order_relaxed);
    }

    std::uint64_t PublishedCount() const {
        return m_write.load(std::memory_order_acquire);
    }

    std::uint64_t ConsumedCount() const {
        return m_read.load(std::memory_order_acquire);
    }

    // The most records any one quantum has produced, budget refusals excluded
    // (they were never produced). This is the figure the budget is sized
    // against, and it is not the ring's occupancy: occupancy also depends on
    // when the consumer drained.
    std::uint32_t QuantumProducedHighWater() const {
        return m_producedHighWater;
    }

private:
    void PushUnbudgeted(const LogRecord& record) {
        LogRecord stamped = record;
        if (stamped.timestampNs == 0)
            stamped.timestampNs = LogTimestampNs();
        std::uint64_t words[8];
        std::memcpy(words, &stamped, sizeof words);
        const std::uint64_t write = m_write.load(std::memory_order_relaxed);
        Slot& slot = m_slots[write & kMask];
        for (std::size_t i = 0; i < std::size(words); ++i) {
            slot.words[i].store(words[i], std::memory_order_release);
        }
        m_write.store(write + 1, std::memory_order_release);
    }

public:
    // Consumer side (Vn_Update). Skips over any overwritten span first,
    // adding its length to the drop counter, then reads the oldest
    // surviving record and revalidates the copy. Returns false when drained
    // or when the fixed invalidated-copy budget is exhausted.
    //
    // Survivor window: the producer may be mid-writing the record at its
    // current write index, and that write reclaims the slot of record
    // (write - kCapacity). A record is therefore proven untouched during
    // the copy only when the POST-copy write index satisfies
    // write - index < kCapacity (the producer bumps the index only after
    // finishing a slot write). The boundary record at lag == kCapacity is
    // indeterminate and conservatively dropped — the ring safely drains
    // the newest kCapacity - 1 records after an overflow.
    bool TryPop(LogRecord& out, std::uint64_t* sourceSequence = nullptr) {
        constexpr std::uint32_t kMaxInvalidatedCopies = 4;
        std::uint64_t read = m_read.load(std::memory_order_relaxed);
        std::uint64_t write = m_write.load(std::memory_order_acquire);
        // How full the ring was when this drain reached it. Measured here, on
        // the consumer, because both indices are readable from this side --
        // the producer never has to compute it and the audio thread is
        // untouched. The lag can exceed capacity when the producer has lapped
        // the reader; occupancy is what the ring can actually hold, so it
        // saturates. The drop counter says overflow already happened; this says
        // how much headroom was left when it did not, which is the only one of
        // the two a caller can size a budget from.
        const std::uint64_t lag = write - read;
        const std::uint32_t occupancy = lag >= kCapacity
                                            ? kCapacity
                                            : static_cast<std::uint32_t>(lag);
        if (occupancy > m_occupancyHighWater) {
            m_occupancyHighWater = occupancy;
        }
        std::uint32_t invalidatedCopies = 0;
        for (;;) {
            if (read == write) {
                return false;
            }
            if (write - read >= kCapacity) {
                const std::uint64_t resync = write - kCapacity + 1;
                m_dropped += resync - read;
                read = resync;
                continue;
            }
            Slot& slot = m_slots[read & kMask];
            std::uint64_t words[8];
            for (std::size_t i = 0; i < std::size(words); ++i) {
                words[i] = slot.words[i].load(std::memory_order_acquire);
            }
            LogRecord candidate;
            std::memcpy(&candidate, words, sizeof words);
            // Revalidation: if the lag reached capacity while the slot
            // was being copied, the producer may have reclaimed it
            // mid-copy. Discard it and retry within the fixed budget.
            const std::uint64_t writeAfter =
                m_write.load(std::memory_order_acquire);
            if (writeAfter - read < kCapacity) {
                m_read.store(read + 1, std::memory_order_release);
                out = candidate;
                if (sourceSequence != nullptr) *sourceSequence = read + 1;
                return true;
            }
            write = writeAfter;
            if (++invalidatedCopies >= kMaxInvalidatedCopies) {
                m_read.store(read, std::memory_order_release);
                return false;
            }
        }
    }

    // Total records lost to the drop-oldest policy, accumulated at drain
    // time. Consumer-side state, exposed through the snapshot.
    std::uint64_t DroppedCount() const { return m_dropped; }

    // The fullest this ring has been observed at a drain, in records, saturated
    // at kCapacity. Consumer-side state like the drop counter, and exposed the
    // same way. Zero for a session whose drains always found it empty.
    std::uint32_t OccupancyHighWater() const { return m_occupancyHighWater; }

private:
    static constexpr std::uint32_t kMask = kCapacity - 1;

    struct Slot {
        std::atomic<std::uint64_t> words[8]{};
    };
    static_assert(sizeof(Slot) == sizeof(LogRecord),
                  "slot storage must match the log record size");
    static_assert(alignof(Slot) == alignof(std::uint64_t),
                  "slot storage must retain uint64_t alignment");

    Slot m_slots[kCapacity];
    std::atomic<std::uint64_t> m_write{0}; // producer-owned
    std::atomic<std::uint64_t> m_read{0};  // consumer-owned
    std::uint64_t m_dropped = 0;           // consumer-side accumulation
    std::uint32_t m_occupancyHighWater = 0; // consumer-side accumulation
    // Producer-side quantum budget state. Written only by the thread that
    // pushes and opens quanta -- the audio thread for the ring that enables the
    // budget -- and read by the logic thread through the snapshot, which is the
    // same shape as the per-block command-deferral counters already published
    // from the mixer.
    bool m_quantumBudgetEnabled = false;
    std::uint32_t m_producedThisQuantum = 0;
    std::uint32_t m_producedHighWater = 0;
    std::atomic<std::uint64_t> m_quantumBudgetDropped{0};
};

} // namespace harken

#endif // HARKEN_CORE_LOG_RING_H
