// The mixer loop is the audio-thread block renderer: the device callback (or
// the null backend's Pump) calls RenderBlock once per block. Per block it
// drains the command queue, resets the bump arena, runs every rendering
// voice's cascade into its bus's accumulator, folds the bus tree bottom-up
// into the master, and interleaves master into the output buffer. Arena
// exhaustion anywhere in the block raises one degradation notification.
#ifndef HARKEN_RENDER_MIXER_LOOP_H
#define HARKEN_RENDER_MIXER_LOOP_H

#include "harken/core/BlockArena.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/ActionSeekTable.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/MeterBlockRing.h"
#include "harken/core/NotificationQueue.h"
#include "harken/core/LogRing.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/core/SourceCursorMirror.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/TimelineExecutor.h"
#include "harken/core/VariableSliceTable.h"
#include "harken/render/BusTree.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/VoicePool.h"
#include "harken/render/OriginalSourceSubmission.h"
#include "harken/render/VoiceTimelineSink.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {
namespace detail { struct OriginalControlOriginTestAccess; }

class MixerLoop {
public:
    MixerLoop(const EngineConfig& config, VoicePool& voices, BusTree& buses,
              DynamicRouteSystem& dynamicRoutes, CommandQueue& commands,
              ActionSeekTable& actionSeeks,
              StreamSeekHandoffTable& streamSeekHandoffs,
              NotificationQueue& notifications, LogRing& logRing,
              SourceCursorMirror& sourceCursors, MeterBlockRing& meterRing,
              TimelineArmTable* timelineArms = nullptr,
              TimelineExecutor* timelineExecutor = nullptr,
              VoiceTimelineSink* timelineSink = nullptr,
              ScheduledTransactionPool* timelinePool = nullptr,
              VariableSliceTable* variableSlices = nullptr,
              const ProjectGenerationInstallAckIdentityV1* ackSideTable = nullptr,
              std::uint32_t ackSideTableCapacity = 0,
              ProjectGenerationInstallAckRingV1* installAckRing = nullptr,
              OriginalSourceSubmissionRegistry* originalSources = nullptr);

    // Renders one block of interleaved output. Runs under the allocation
    // guard's audio-thread tag: no system heap, no locks, no I/O.
    void RenderBlock(float* interleavedOut, std::uint32_t frames,
                     std::uint32_t channels);

    std::uint64_t BlocksRendered() const {
        return m_blocksRendered.load(std::memory_order_acquire);
    }
    const BlockArena& Arena() const { return m_arena; }
    BlockArena& ArenaForTests() { return m_arena; }

    // The audio-published observability payload: RenderBlock publishes
    // into it at the end of every block; Vn_CaptureSnapshot reads the
    // stable half through Capture().
    const SnapshotBuffer& Snapshots() const { return m_snapshots; }

    // The ceiling in force for the next block's drain. Admission has to refuse
    // an indivisible batch this cannot take, so it must ask the drain rather
    // than read the config the drain was seeded from: two readings of one rule
    // can disagree, and the disagreement admits a batch that then defers for
    // ever. This accessor is the single source.
    std::uint32_t MaxCommandsPerQuantum() const {
        return m_maxCommandsPerQuantum;
    }

    // --- deterministic test hooks ---
    // The per-quantum command ceiling now has a public config field, so a test
    // that only needs a ceiling for the whole session should set that instead of
    // reaching in here. What this hook still gives is the one thing a
    // construction-time field cannot: changing the ceiling between two blocks of
    // one live session. Same discipline as the stream throttle hooks: set before
    // the block whose drain it should bound.
    //
    // This is no longer only a drain-side knob. Admission reads the same value,
    // so lowering it here also narrows what an indivisible batch may be, and a
    // caller's pack load can go from VN_OK to VN_INVALID_CONFIG because of it --
    // this hook can change a public return code.
    //
    // It is a plain write to a field two threads read, so it is only safe
    // because the Null backend pumps blocks on the caller's own thread: the
    // write and every read it affects are ordered by being the same thread.
    // A test driving a real device callback may not use it.
    void SetMaxCommandsPerQuantumForTest(std::uint32_t maxCommands) {
        m_maxCommandsPerQuantum = maxCommands;
    }
    // The render-worker rendezvous budget in force, in performance-counter
    // ticks. 0 only when the counter rate is unavailable.
    std::uint64_t JobDeadlineTicks() const { return m_jobDeadlineTicks; }

    // Records the last drain declined to apply, and the most ever declined.
    std::uint32_t CommandsDeferredLastBlock() const {
        return m_commandsDeferred;
    }
    std::uint64_t CommandsDeferredHighWater() const {
        return m_commandsDeferredHighWater;
    }
    std::uint64_t CommandProtocolViolationCount() const {
        return m_commandProtocolViolations;
    }
    std::size_t PendingControlAckBytesForTest() const noexcept {
        return sizeof(m_pendingControlAck);
    }
    void DrainCommandsForTest() {
        (void)DrainCommands(m_blocksRendered.load(std::memory_order_relaxed),
                            m_commands.PublishedHead());
    }
    std::uint32_t DrainCommandsForTest(std::uint64_t blockIndex) {
        return DrainCommands(blockIndex, m_commands.PublishedHead());
    }
    // Applies queued control, including opcode 15, without rendering.
    void DrainQueuedCommands(std::uint32_t quantumCount);
    // Engine teardown only, after queue discard and callback/worker join.
    void FinishControlHeaderAckAfterQuiescence() noexcept;
    void SuppressCommandProtocolAssertForTest(bool suppress) {
        m_suppressCommandProtocolAssertForTest = suppress;
    }
    enum class CurrentSliceHookPhaseForTest : std::uint8_t {
        AfterStageBeforeExecute = 0,
        AfterExecuteBeforeApply = 1,
    };
    using CurrentSliceFrontierHookForTest = void (*)(
        void*, CurrentSliceHookPhaseForTest, std::uint64_t, std::size_t,
        std::uint32_t, TimelinePagePrefixStopReason) noexcept;
    void SetCurrentSliceFrontierHookForTest(
        CurrentSliceFrontierHookForTest hook, void* context) noexcept {
        m_currentSliceFrontierHookForTest = hook;
        m_currentSliceFrontierHookContextForTest = context;
    }
    void SetCurrentSliceScratchLimitForTest(std::size_t limit) noexcept {
        m_currentSliceScratchLimitForTest = limit;
    }
    void RestoreCurrentSliceScratchLimitForTest() noexcept {
        m_currentSliceScratchLimitForTest = m_currentSliceScratch.size();
    }
    std::size_t CurrentSliceScratchCapacityForTest() const noexcept {
        return m_currentSliceScratch.size();
    }
    std::size_t EffectiveCurrentSliceCapacity() const noexcept {
        return std::min(m_currentSliceScratch.size(), m_currentSliceScratchLimitForTest);
    }

private:
    friend struct detail::OriginalControlOriginTestAccess;
    // One block's stage timing, measured inside RenderBlock from QPC reads at
    // the stage boundaries and published with that block's payload.
    struct BlockTiming {
        std::uint32_t wholeUs = 0;
        std::uint32_t drainUs = 0;
        std::uint32_t voicesUs = 0;
        std::uint32_t busesUs = 0;
    };

    // Applies commands published at or before `publishedHead`, at most
    // m_maxCommandsPerQuantum of them, cutting only between atomic batches.
    // Returns how many records were left behind for a later quantum.
    // How long the render-worker rendezvous may take before the coordinator
    // walks away from the batch. An absolute duration, not a share of the block
    // period: what this has to clear is the machine's own scheduling jitter, and
    // that is an absolute quantity — measurements across 128, 256 and 480 frame
    // blocks put the worst observed rendezvous in the same 200-340 us band
    // regardless of block width. Expressed as a share instead, one number would
    // mean 2.2x headroom at 128 frames and 8.6x at 480 for no difference in real
    // risk, and every recalibration would have to be redone per block size.
    //
    // A safe starting point, not a measured optimum: about three times the worst
    // span observed on an idle machine with a null backend, which is a lower
    // bound on a lower bound — it had no real-time competition, no device, and
    // did not include worker wake latency. It wants calibration on target
    // hardware.
    static constexpr std::uint64_t kJobDeadlineMicros = 1000;
    // The block period is a ceiling guard only. A budget that outlasts the block
    // it belongs to would let one late block push the next one late as well, so
    // the absolute figure is capped here — but the cap never raises it, because
    // a budget below the jitter floor would abandon healthy batches. Measured
    // worst case already occupies 11.5% of the shortest block period, with 7.5x
    // to 9.7x spread above the median, so anything under about a quarter of the
    // period is known-wrong; half leaves room above that.
    static constexpr std::uint64_t kJobDeadlineBlockPeriodPercent = 50;

    std::uint32_t DrainCommands(std::uint64_t blockIndex,
                                std::uint64_t publishedHead);
    bool DrainOriginalSource(std::uint64_t blockIndex, std::uint64_t& owed,
        std::uint32_t& applied, std::uint32_t remaining) noexcept;
    void ApplyCommandRecord(const CommandRecord& command,
                            std::uint64_t blockIndex) noexcept;
    struct CurrentSliceStagePlan {
        std::uint64_t expectedFrontier = 0;
        std::uint32_t stagedPageCount = 0;
        std::size_t sliceEndAfterPage[2]{};
        TimelinePagePrefixStopReason stopReason =
            TimelinePagePrefixStopReason::Unavailable;
    };

    CurrentSliceStagePlan StageCurrentSlices(
        std::uint64_t blockIndex) noexcept;
    void ApplyStagedCurrentSlices(
        std::uint64_t blockIndex, const CurrentSliceStagePlan& plan,
        const TimelineExecutionResult& result) noexcept;
    void PublishSnapshot(std::uint64_t blockIndex, bool blockRendered,
                         const BlockTiming& timing);

    const std::uint32_t m_maxBuses;
    const std::uint32_t m_audibilityParkEnabled;
    // The per-quantum command ceiling. Read by two threads -- the audio thread
    // each block in DrainCommands, and the logic thread on every command
    // admission through MaxCommandsPerQuantum() -- and written by neither once
    // constructed, which is why a plain integer is sufficient today. It is not
    // const only because the test hook below writes it.
    //
    // The public config field is not a second writer: it is read once in the
    // constructor, before any callback exists, so it leaves the "written by
    // neither once constructed" property intact. What would break that property
    // is retuning the ceiling while a callback is live -- any such writer makes
    // this a mutable cross-thread read and must make the field atomic first.
    std::uint32_t m_maxCommandsPerQuantum;
    // Records this block declined to apply, and the most ever declined at once.
    // Written by the audio thread each block, read by the logic thread through
    // the published snapshot.
    std::uint32_t m_commandsDeferred = 0;
    std::uint64_t m_commandsDeferredHighWater = 0;
    std::uint64_t m_commandProtocolViolations = 0;
    std::uint32_t m_commandBatchDiscardRemaining = 0;
    bool m_suppressCommandProtocolAssertForTest = false;
    // Consecutive blocks that left work behind, and whether this episode has
    // already been reported. Both reset the moment a block catches up, so a
    // later episode reports again instead of the signal going quiet for the rest
    // of the session after the first one.
    std::uint32_t m_consecutiveDeferredBlocks = 0;
    bool m_deferralEpisodeReported = false;
    // Consecutive blocks behind before the backlog counts as sustained rather
    // than a burst a prebuffered queue absorbs on its own.
    static constexpr std::uint32_t kDeferralEpisodeBlocks = 8;
    VoicePool& m_voices;
    BusTree& m_buses;
    DynamicRouteSystem& m_dynamicRoutes;
    CommandQueue& m_commands;
    ActionSeekTable& m_actionSeeks;
    StreamSeekHandoffTable& m_streamSeekHandoffs;
    std::vector<std::uint64_t> m_seekResolvedFrames;
    std::vector<StreamSeekHandoffSnapshot> m_seekStreamHandoffs;
    NotificationQueue& m_notifications;
    LogRing& m_logRing;
    // Per-slot source-cursor mirror the block loop publishes for audibility
    // parking's timeline authority. Owned by the engine; the mixer only writes.
    SourceCursorMirror& m_sourceCursors;
    // Meter block ring: one record of all three bus tap rows per rendered
    // block while any meter Variable exists. Owned by the engine;
    // the mixer only appends. The armed gate keeps the unarmed cost to one
    // relaxed load per block.
    MeterBlockRing& m_meterRing;
    OriginalSourceSubmissionRegistry* m_originalSources = nullptr;
    TimelineArmTable* m_timelineArms = nullptr;
    struct PendingControlAck {
        TimelineArmTable* owner = nullptr;
        std::uint64_t incarnation = 0;
        TimelineArmToken token{};
        std::uint64_t originalCompletionSequence = 0;
        CommandQueue::OriginalBatchRange* originalRange = nullptr;
    };
    PendingControlAck m_pendingControlAck{};
    // Stable native actor remains across a real busy ACK, not one callback.
    CommandQueue::OriginalControlMutationActor m_pendingControlOriginActor;
    bool RetryPendingControlAck(std::uint64_t blockIndex) noexcept;
    bool DrainOriginalControlHeader(std::uint64_t blockIndex, std::uint64_t& owed,
        std::uint32_t& applied) noexcept;
    TimelineExecutor* m_timelineExecutor = nullptr;
    VoiceTimelineSink* m_timelineSink = nullptr;
    ScheduledTransactionPool* m_timelinePool = nullptr;
    VariableSliceTable* m_variableSlices = nullptr;
    const ProjectGenerationInstallAckIdentityV1* m_installAckSideTable = nullptr;
    std::uint32_t m_installAckSideTableCapacity = 0;
    ProjectGenerationInstallAckRingV1* m_installAckRing = nullptr;
    std::vector<ConsumedCurrentSlice> m_currentSliceScratch;
    std::size_t m_stagedCurrentSliceCount = 0;
    std::size_t m_currentSliceScratchLimitForTest = 0;
    CurrentSliceFrontierHookForTest m_currentSliceFrontierHookForTest = nullptr;
    void* m_currentSliceFrontierHookContextForTest = nullptr;
    // Preallocated record scratch for the three meter tap lanes,
    // written and handed to PushTaps() on the audio thread — sized at init, so
    // the append allocates nothing.
    std::vector<float> m_meterTapScratch;
    std::vector<std::uint32_t> m_meterGenerationScratch;
    SnapshotBuffer m_snapshots;
    BlockArena m_arena;
    // Per-bus accumulator plane pointers, bus-major (m_mixChannels per bus).
    // Sized once at init; the audio thread only rewrites the pointers each
    // block — the planes themselves come from the arena.
    std::vector<float*> m_busPlanes;
    // Mix and device layouts (resolved config). The bus tree renders at the mix
    // width; after the fold, RenderBlock lays the master's mix-width output into
    // the device buffer — direct interleave when the device layout matches the
    // mix, the master's downmix matrix when the device is narrower, name
    // placement by role when it is wider. Stereo == stereo at the default, at
    // which the output is byte-identical to M4.
    std::uint32_t m_mixChannels;
    std::uint32_t m_deviceChannels;
    std::uint32_t m_mixLayout;
    std::uint32_t m_deviceLayout;
    std::uint32_t m_sampleRate;
    std::atomic<std::uint64_t> m_blocksRendered{0};
    // QPC tick rate, fetched once at construction (logic thread) so the
    // audio thread only ever reads the cached value.
    std::uint64_t m_cpuTicksPerSecond = 0;
    // The render-worker rendezvous budget, resolved once on the logic thread
    // from the two figures above and the frozen block width. The audio thread
    // only reads it.
    std::uint64_t m_jobDeadlineTicks = 0;
    // Highest whole-callback microseconds observed since init; audio-thread
    // owned, published with every block.
    std::uint32_t m_audioCpuPeakUs = 0;
};

} // namespace harken

#endif // HARKEN_RENDER_MIXER_LOOP_H
