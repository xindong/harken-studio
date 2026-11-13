// Per-stream SPSC byte storage. The producer publishes verified encoded bytes;
// only the renderer converts them to samples. Frame counters separately track
// playable supply and frames delivered to the resampler, including decoded
// samples whose compressed packet has already left the byte ring.
#ifndef HARKEN_CORE_STREAM_RING_H
#define HARKEN_CORE_STREAM_RING_H

#include "harken/core/EngineConfig.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cassert>

namespace harken {
class MediaDecoder;

enum class StreamSlotState : std::uint32_t {
    Free = 0,    // unclaimed; invisible to both real-time threads
    Active = 1,  // wired to a voice; producer feeds, consumer drains
    Retired = 2  // voice released; producer detaches, then frees the slot
};

// Why a stream stopped producing. ProducerFailure is not a content outcome: the
// streaming thread died, so buffered frames are valid but nothing further is
// coming. It is kept distinct from NaturalEnd (which would claim the source
// finished) and from IntegrityAbort (which would claim the data was corrupt and
// send an integrator off to replace a file that is fine).
//
// Count is the tripwire for the mappings below: appending a reason and bumping it
// fails their static_asserts, so the mapping is updated deliberately rather than
// a new value silently landing in whatever branch happens to be last.
enum class StreamTerminalReason : std::uint32_t {
    None = 0,
    NaturalEnd = 1,
    IntegrityAbort = 2,
    ProducerFailure = 3,
    Count = 4
};

enum class StreamLifecycleWinner : std::uint32_t {
    Open = 0,
    NaturalEnd = 1,
    IntegrityAbort = 2,
    ParkPending = 3,
    Park = 4,
    ProducerFailure = 5,
    Count = 6
};

static_assert(static_cast<std::uint32_t>(StreamTerminalReason::Count) == 4,
              "a terminal reason was added: extend TerminalReasonForWinner and "
              "LifecycleWinnerForTerminalReason here, and "
              "ParkDispositionForTerminalReason in StreamParkMailbox.h, "
              "before bumping this");
static_assert(static_cast<std::uint32_t>(StreamLifecycleWinner::Count) == 6,
              "a lifecycle winner was added: extend TerminalReasonForWinner "
              "before bumping this");

// The one place a lifecycle winner becomes a terminal reason. Three separate
// ternary chains used to do this, which is how a newly added reason could be
// swallowed by the trailing branch of each. Exhaustive switch with no default,
// so the compiler flags an unhandled enumerator instead.
inline StreamTerminalReason TerminalReasonForWinner(
    StreamLifecycleWinner winner) {
    switch (winner) {
    case StreamLifecycleWinner::NaturalEnd:
        return StreamTerminalReason::NaturalEnd;
    case StreamLifecycleWinner::IntegrityAbort:
        return StreamTerminalReason::IntegrityAbort;
    case StreamLifecycleWinner::ProducerFailure:
        return StreamTerminalReason::ProducerFailure;
    case StreamLifecycleWinner::Open:
    case StreamLifecycleWinner::ParkPending:
    case StreamLifecycleWinner::Park:
    case StreamLifecycleWinner::Count:
        return StreamTerminalReason::None;
    }
    return StreamTerminalReason::None;
}

// The inverse, for publishing a terminal outcome.
inline StreamLifecycleWinner LifecycleWinnerForTerminalReason(
    StreamTerminalReason reason) {
    switch (reason) {
    case StreamTerminalReason::IntegrityAbort:
        return StreamLifecycleWinner::IntegrityAbort;
    case StreamTerminalReason::ProducerFailure:
        return StreamLifecycleWinner::ProducerFailure;
    case StreamTerminalReason::NaturalEnd:
    case StreamTerminalReason::None:
    case StreamTerminalReason::Count:
        return StreamLifecycleWinner::NaturalEnd;
    }
    return StreamLifecycleWinner::NaturalEnd;
}

struct StreamRing {
    std::uint8_t* bytes = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t mask = 0;
    std::uint32_t channelCount = 0;
    std::uint32_t bytesPerFrame = 0;
    std::uint32_t legacyFrameLimit = 0;
    bool packetEncoded = false;
    // Logic publishes once for an initially unprepared encoded source. Only
    // its renderer takes ownership while Active; detach retires an unclaimed
    // value before publishing Free. A consumed nullptr is never republished.
    std::atomic<MediaDecoder*> pendingDecoder{nullptr};
    std::atomic<std::uint32_t> filledFrameLimit{UINT32_MAX};
    std::atomic<VnResult> preparationResult{VN_OK};
    std::atomic<std::uint64_t> writeIndex{0};
    std::atomic<std::uint64_t> readIndex{0};
    std::atomic<std::uint64_t> writeFrames{0};
    std::atomic<std::uint64_t> readFrames{0};

    // The producer and audio thread compete on one generation-bound winner.
    // Producer terminal publication is release-ordered after the final write;
    // an audio-side park claim prevents a later producer terminal from
    // reopening or changing the retired generation.
    std::atomic<StreamLifecycleWinner> lifecycleWinner{
        StreamLifecycleWinner::Open};
    std::atomic<std::uint32_t> generation{1};
    std::atomic<std::uint32_t> winnerGeneration{0};

    std::atomic<StreamSlotState> state{StreamSlotState::Free};
    // Logic publishes steady-I/O admission separately from ring ownership.
    // Initial timeline starts observe this exact generation's promotion.
    std::atomic<bool> startPromoted{false};
    // One exact replacement application's audio receipt, reset with this ring.
    std::atomic<std::uint64_t> appliedFrame{UINT64_MAX};
    std::atomic<std::uint64_t> appliedAudioBlock{0};
    std::atomic<std::uint64_t> appliedReplacementSerial{0};
    std::atomic<std::uint64_t> observationRender{0};
    // Immutable for the Active generation; never reused before audio retirement.
    std::uint64_t sourceGeneration = 0;
    std::uint64_t contentRevision = 0;

    void Wire(std::uint8_t* storage, std::uint32_t byteCapacity,
              std::uint32_t frameLimit = 0) {
        assert(byteCapacity != 0 && (byteCapacity & (byteCapacity - 1)) == 0);
        bytes = storage; capacity = byteCapacity; mask = byteCapacity - 1;
        legacyFrameLimit = frameLimit;
    }

    // Claim-time reset, logic thread only, while state is Free — neither
    // real-time thread observes a Free slot, so plain stores suffice until
    // the Active publication (a release store by the caller) makes the
    // whole wiring visible.
    void ResetForUse(std::uint32_t channels, bool encoded = false) {
        assert(pendingDecoder.load(std::memory_order_relaxed) == nullptr);
        std::uint32_t next =
            generation.load(std::memory_order_relaxed) + 1u;
        if (next == 0) {
            next = 1;
        }
        generation.store(next, std::memory_order_relaxed);
        startPromoted.store(false, std::memory_order_relaxed);
        appliedFrame.store(UINT64_MAX, std::memory_order_relaxed);
        appliedAudioBlock.store(0, std::memory_order_relaxed);
        appliedReplacementSerial.store(0, std::memory_order_relaxed);
        observationRender.store(0, std::memory_order_relaxed);
        channelCount = channels;
        packetEncoded = encoded;
        filledFrameLimit.store(UINT32_MAX, std::memory_order_relaxed);
        preparationResult.store(VN_OK, std::memory_order_relaxed);
        bytesPerFrame = channels * sizeof(float);
        writeFrames.store(0, std::memory_order_relaxed);
        readFrames.store(0, std::memory_order_relaxed);
        writeIndex.store(0, std::memory_order_relaxed);
        readIndex.store(0, std::memory_order_relaxed);
        winnerGeneration.store(next, std::memory_order_relaxed);
        lifecycleWinner.store(StreamLifecycleWinner::Open,
                              std::memory_order_relaxed);
    }

    std::uint32_t WriteAvailableBytes() const {
        const auto w = writeIndex.load(std::memory_order_relaxed);
        const auto r = readIndex.load(std::memory_order_acquire);
        const auto limit = legacyFrameLimit && !packetEncoded
            ? std::min<std::uint64_t>(capacity, std::uint64_t(legacyFrameLimit) * bytesPerFrame)
            : capacity;
        return static_cast<std::uint32_t>(limit - (w - r));
    }

    // Copying and publication are separate so a complete packet or PCM frame
    // becomes visible only after every wrapped byte has been written.
    void CopyIn(std::uint64_t at, const void* source, std::uint32_t count) {
        assert(count <= capacity);
        const auto pos = static_cast<std::uint32_t>(at) & mask;
        const auto first = std::min(count, capacity - pos);
        std::memcpy(bytes + pos, source, first);
        std::memcpy(bytes, static_cast<const std::uint8_t*>(source) + first, count - first);
    }
    void CopyOut(std::uint64_t at, void* destination, std::uint32_t count) const {
        assert(count <= capacity);
        const auto pos = static_cast<std::uint32_t>(at) & mask;
        const auto first = std::min(count, capacity - pos);
        std::memcpy(destination, bytes + pos, first);
        std::memcpy(static_cast<std::uint8_t*>(destination) + first, bytes, count - first);
    }
    void PublishBytes(std::uint32_t count, std::uint32_t frames) {
        writeIndex.store(writeIndex.load(std::memory_order_relaxed) + count, std::memory_order_release);
        writeFrames.store(writeFrames.load(std::memory_order_relaxed) + frames, std::memory_order_release);
    }
    // Admission can query a free slot before its next generation is reset.
    std::uint32_t FrameCapacity(std::uint32_t channels = 0, bool encoded = false) const {
        if (encoded || (!channels && packetEncoded))
            return legacyFrameLimit ? legacyFrameLimit : UINT32_MAX;
        const auto stride = channels ? channels * sizeof(float) : bytesPerFrame;
        const auto frames = stride ? static_cast<std::uint32_t>(capacity / stride) : 0;
        return legacyFrameLimit ? std::min(frames, legacyFrameLimit) : frames;
    }
    std::uint32_t PrebufferTarget(std::uint32_t requested) const noexcept {
        return std::min(requested, filledFrameLimit.load(std::memory_order_acquire));
    }

    bool MarkEndOfData(StreamTerminalReason reason) {
        const StreamLifecycleWinner desired =
            LifecycleWinnerForTerminalReason(reason);
        winnerGeneration.store(
            generation.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        StreamLifecycleWinner expected = StreamLifecycleWinner::Open;
        if (lifecycleWinner.compare_exchange_strong(
                expected, desired, std::memory_order_release,
                std::memory_order_acquire)) {
            return true;
        }
        if (expected != StreamLifecycleWinner::ParkPending) {
            return false;
        }
        return lifecycleWinner.compare_exchange_strong(
            expected, desired, std::memory_order_release,
            std::memory_order_acquire);
    }

    std::uint32_t ReadAvailableBytes() const {
        const auto w = writeIndex.load(std::memory_order_acquire);
        const auto r = readIndex.load(std::memory_order_relaxed);
        return static_cast<std::uint32_t>(w - r);
    }
    std::uint32_t BufferedFrames() const {
        const auto r = readFrames.load(std::memory_order_acquire);
        const auto w = writeFrames.load(std::memory_order_acquire);
        return w > r ? static_cast<std::uint32_t>(std::min<std::uint64_t>(w - r, UINT32_MAX)) : 0;
    }
    void ConsumeBytes(std::uint32_t count) {
        readIndex.store(readIndex.load(std::memory_order_relaxed) + count, std::memory_order_release);
    }
    void DeliverFrames(std::uint32_t frames) {
        readFrames.store(readFrames.load(std::memory_order_relaxed) + frames, std::memory_order_release);
    }

    bool SourceEnded() const {
        return TerminalReason() != StreamTerminalReason::None;
    }

    StreamTerminalReason TerminalReason() const {
        const StreamLifecycleWinner winner =
            lifecycleWinner.load(std::memory_order_acquire);
        if (winnerGeneration.load(std::memory_order_relaxed) !=
            generation.load(std::memory_order_relaxed)) {
            return StreamTerminalReason::None;
        }
        return TerminalReasonForWinner(winner);
    }

    bool TryClaimPark(StreamTerminalReason* terminalWinner) {
        if (terminalWinner == nullptr) {
            return false;
        }
        winnerGeneration.store(
            generation.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        StreamLifecycleWinner expected = StreamLifecycleWinner::Open;
        if (lifecycleWinner.compare_exchange_strong(
                expected, StreamLifecycleWinner::ParkPending,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            *terminalWinner = StreamTerminalReason::None;
            return true;
        }
        *terminalWinner = TerminalReasonForWinner(expected);
        return false;
    }

    bool TryCommitPark(StreamTerminalReason* terminalWinner) {
        if (terminalWinner == nullptr) {
            return false;
        }
        StreamLifecycleWinner expected = StreamLifecycleWinner::ParkPending;
        if (lifecycleWinner.compare_exchange_strong(
                expected, StreamLifecycleWinner::Park,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            *terminalWinner = StreamTerminalReason::None;
            return true;
        }
        *terminalWinner = TerminalReasonForWinner(expected);
        return false;
    }
};

static_assert(std::atomic<StreamLifecycleWinner>::is_always_lock_free,
              "stream lifecycle arbitration must be lock-free");

} // namespace harken

#endif // HARKEN_CORE_STREAM_RING_H
