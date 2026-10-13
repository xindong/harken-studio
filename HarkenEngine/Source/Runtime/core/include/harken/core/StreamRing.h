// Per-stream SPSC frame ring: the only channel between the streaming
// thread (producer) and the audio thread (consumer). Planar float frames
// over preallocated storage, power-of-two capacity, absolute uint64
// indices with release/acquire pairing — the same lock-free discipline as
// the command queue, carrying sample frames instead of records.
//
// This lives in core because both sides consume it across the module
// boundary: media owns the slots and feeds them, render reads them. The
// audio thread never blocks, allocates, or locks here; the producer's only
// coupling to the consumer is the read index it observes.
//
// The ring also carries the slot's lifecycle word, because the audio
// thread must be able to give the slot back without touching any
// logic-side structure: Free -> Active is published by the logic thread
// once the slot is fully wired (release), Active -> Retired is stored by
// the audio thread when the voice releases (release), and the streaming
// thread detaches a Retired slot and returns it to Free.
#ifndef HARKEN_CORE_STREAM_RING_H
#define HARKEN_CORE_STREAM_RING_H

#include "harken/core/EngineConfig.h"

#include <atomic>
#include <cstdint>

namespace harken {

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
    // One plane per channel, `capacity` floats each, wired once at init
    // into pool-owned storage. Only the first channelCount planes carry
    // data for the current stream.
    float* planes[kMaxChannels] = {};
    std::uint32_t capacity = 0; // power of two
    std::uint32_t mask = 0;
    std::uint32_t channelCount = 0;

    // Absolute frame counts; at 48 kHz a uint64 cannot wrap in practice.
    // writeIndex is producer-owned (consumer acquires), readIndex is
    // consumer-owned (producer acquires).
    std::atomic<std::uint64_t> writeIndex{0};
    std::atomic<std::uint64_t> readIndex{0};

    // The producer and audio thread compete on one generation-bound winner.
    // Producer terminal publication is release-ordered after the final write;
    // an audio-side park claim prevents a later producer terminal from
    // reopening or changing the retired generation.
    std::atomic<StreamLifecycleWinner> lifecycleWinner{
        StreamLifecycleWinner::Open};
    std::atomic<std::uint32_t> generation{1};
    std::atomic<std::uint32_t> winnerGeneration{0};

    std::atomic<StreamSlotState> state{StreamSlotState::Free};

    // Init-time wiring; both real-time threads exist only after this.
    void Wire(float* const* storage, std::uint32_t cap) {
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
            planes[ch] = storage[ch];
        }
        capacity = cap;
        mask = cap - 1;
    }

    // Claim-time reset, logic thread only, while state is Free — neither
    // real-time thread observes a Free slot, so plain stores suffice until
    // the Active publication (a release store by the caller) makes the
    // whole wiring visible.
    void ResetForUse(std::uint32_t channels) {
        std::uint32_t next =
            generation.load(std::memory_order_relaxed) + 1u;
        if (next == 0) {
            next = 1;
        }
        generation.store(next, std::memory_order_relaxed);
        channelCount = channels;
        writeIndex.store(0, std::memory_order_relaxed);
        readIndex.store(0, std::memory_order_relaxed);
        winnerGeneration.store(next, std::memory_order_relaxed);
        lifecycleWinner.store(StreamLifecycleWinner::Open,
                              std::memory_order_relaxed);
    }

    // --- producer side (streaming thread) ---

    std::uint32_t WriteAvailable() const {
        const std::uint64_t w = writeIndex.load(std::memory_order_relaxed);
        const std::uint64_t r = readIndex.load(std::memory_order_acquire);
        return capacity - static_cast<std::uint32_t>(w - r);
    }

    // Largest contiguous span writable from the current write position:
    // bounded by free space and by the wrap point. Fills outPlanes with
    // the write cursors of the first channelCount planes.
    std::uint32_t ContiguousWrite(float** outPlanes) {
        const std::uint32_t avail = WriteAvailable();
        const std::uint32_t pos = static_cast<std::uint32_t>(
                                      writeIndex.load(
                                          std::memory_order_relaxed)) &
                                  mask;
        const std::uint32_t untilWrap = capacity - pos;
        const std::uint32_t span = avail < untilWrap ? avail : untilWrap;
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            outPlanes[ch] = planes[ch] + pos;
        }
        return span;
    }

    void PublishWrite(std::uint32_t frames) {
        writeIndex.store(writeIndex.load(std::memory_order_relaxed) + frames,
                         std::memory_order_release);
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

    // --- consumer side (audio thread) ---

    std::uint32_t ReadAvailable() const {
        const std::uint64_t w = writeIndex.load(std::memory_order_acquire);
        const std::uint64_t r = readIndex.load(std::memory_order_relaxed);
        return static_cast<std::uint32_t>(w - r);
    }

    // Largest contiguous span readable from the current read position.
    std::uint32_t ContiguousRead(const float** outPlanes) const {
        const std::uint32_t avail = ReadAvailable();
        const std::uint32_t pos = static_cast<std::uint32_t>(
                                      readIndex.load(
                                          std::memory_order_relaxed)) &
                                  mask;
        const std::uint32_t untilWrap = capacity - pos;
        const std::uint32_t span = avail < untilWrap ? avail : untilWrap;
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            outPlanes[ch] = planes[ch] + pos;
        }
        return span;
    }

    void AdvanceRead(std::uint32_t frames) {
        readIndex.store(readIndex.load(std::memory_order_relaxed) + frames,
                        std::memory_order_release);
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
