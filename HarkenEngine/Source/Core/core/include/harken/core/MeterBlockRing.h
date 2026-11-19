// Meter block ring (audio -> logic): the SPSC transport behind bus meter
// Variables. After each rendered
// block, once any meter Variable exists, the audio thread appends one
// fixed-size POD record { blockIndex, generations[maxBuses], three tap
// rows[maxBuses] }; the logic
// thread drains ALL records each Vn_Update and advances the meter
// ballistics per block. The ring preserves every block's peak and its
// position in time — a latest-value snapshot would drop transients between
// ticks and bend ballistics with the game frame rate.
//
// Full-ring policy — a recorded deviation from the section-11.2 mechanism
// sketch ("max-holds into the newest slot"; erratum registered on the
// dashboard): the sketch's in-place merge of a published slot is not
// implementable without a data race or a lost-peak interleaving, so the
// overflowing block max-holds into a DEDICATED OVERFLOW SIDE-RECORD
// instead — one atomic generation+peak lane per tap and bus — and bumps the
// overflow counter. Existence of a compressed span is judged by the COUNTER, not
// by lane values, so a run of all-silent compressed blocks still advances
// ballistics one release step (time compression may fold many blocks into
// one, never into zero). The section's guarantee sentence holds verbatim:
// peaks are never silently lost, at worst time-compressed — the drain
// takes the side-record atomically and advances ballistics one compressed
// block, delivered even when no further append ever happens.
//
// Attribution (why the compressed block needs no per-pairing gate): the
// meter wiring samples the ring's write cursor AT THE ENTRY of every
// pairing call and uses that ONE value as both the entry drain's boundary
// and the new pairing's epoch. The entry drain consumes every record
// before the boundary and takes the side-record (or defers it — below),
// so everything the side-record holds afterwards post-dates the entry of
// every pairing alive to consume it ("from this call on" is defined at
// call entry: a block completing while the call runs sits at or past the
// boundary, survives the entry round as an ordinary record, and reaches
// the new pairing next round). The compressed block therefore delivers
// to all active pairings unconditionally; ordinary records keep their
// exact per-pairing epoch gate (append position vs the entry boundary).
//
// Concurrency argument (no C++ data race anywhere):
//  - Ordinary records are classic SPSC: the producer plain-writes a slot
//    it owns (outside [read, write)) and publishes it with a RELEASE
//    store of the write cursor; the consumer ACQUIRE-loads the cursor
//    before reading, and a published slot is never written again — the
//    overflow path touches only the side-record. Slot reclaim is gated
//    the same way through the read cursor (release on pop, acquire in
//    the producer's full check).
//  - The overflow side-record is atomic end to end: producer stamps the
//    position, CAS-max merges the lanes, then bumps the counter with a
//    release store; the consumer's acquire reads and lane exchanges pair
//    with those. A merge that lands after the exchange waits for a later
//    drain — at-least-once delivery, never loss. Lock-free,
//    allocation-free, and decision-free on the audio side (the CAS loops
//    contend only with the single consumer's exchange and merge-back).
//
// Validated take (how the consumer applies the side-record in order):
// after the ordinary records of a round are consumed, a compressed block
// exists when the overflow counter moved past the consumer's watermark.
// The consumer applies it ONLY when the position stamp does not exceed
// the queue front — i.e. every merge recorded so far precedes everything
// still queued — and re-checks the stamp after exchanging the lanes. If
// records earlier than the newest merge are still queued, the take DEFERS
// untouched to a later round (which consumes those records first); if a
// merge lands inside the take window, the taken lanes are max-merged BACK
// and the whole block defers likewise. Deferral delays delivery by drain
// rounds; it never loses a peak and never applies compressed content
// ahead of records that really precede it.
//
// Bounded degradation while the ring is overflowing (the dashboard
// erratum on sections 11.2/11.3: exact per-block ordering and exact
// pairing epochs degrade BOUNDEDLY during overflow, never silently):
//  - Time compression itself: overflowing blocks fold into one compressed
//    step per drain round, peak max-held — many blocks into one, never
//    into zero, counted by the overflow counter and warned by the log.
//  - Counter-lag split: the producer's counter bump trails its lane
//    merge, so one fold can split into a peak step this round plus one
//    all-silent RELEASE step a later round — release-conservative, the
//    peak itself is never lost.
//  - Deferred attribution: a take deferred across a pairing call (a
//    standing overflow spanning the call) can carry earlier peaks into
//    the new pairing's first compressed block — bounded to that one
//    block, and reachable only while the ring is already overflowing.
//
// Storage is preallocated at construction on the logic thread (capacity
// rounds up to a power of two for mask indexing, matching the ring
// family); push and drain touch no heap and take no locks. The armed gate
// keeps the unarmed per-block cost to one relaxed load.
#ifndef HARKEN_CORE_METER_BLOCK_RING_H
#define HARKEN_CORE_METER_BLOCK_RING_H

#include <harken/Harken.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace harken {

enum class MeterTapLane : std::uint32_t {
    PostEffects = VN_BUS_METER_TAP_POST_EFFECTS,
    PreFader = VN_BUS_METER_TAP_PRE_FADER,
    PostFader = VN_BUS_METER_TAP_POST_FADER,
    Count
};

static_assert(static_cast<std::uint32_t>(MeterTapLane::PostEffects) == 0u);
static_assert(static_cast<std::uint32_t>(MeterTapLane::PreFader) == 1u);
static_assert(static_cast<std::uint32_t>(MeterTapLane::PostFader) == 2u);

inline constexpr std::uint32_t kMeterTapLaneCount =
    static_cast<std::uint32_t>(MeterTapLane::Count);

inline constexpr std::size_t MeterTapOffset(std::uint32_t tap,
                                            std::uint32_t maxBuses) {
    return static_cast<std::size_t>(tap) * maxBuses;
}

class MeterBlockRing {
public:
    // capacityBlocks is the resolved meterRingBlocks config value (already
    // power-of-two rounded by config resolution; rounded again here so a
    // directly constructed ring is safe too). maxBuses fixes the per-record
    // peak span — the record shape is runtime-fixed POD, laid out as parallel
    // preallocated arrays (indices, generation rows, and three-lane peak rows)
    // of identical slot count, plus the atomic overflow side-record.
    MeterBlockRing(std::uint32_t capacityBlocks, std::uint32_t maxBuses)
        : m_maxBuses(maxBuses),
          m_overflowLanes(static_cast<std::size_t>(kMeterTapLaneCount) *
                          maxBuses) {
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                      "the audio-side overflow path must stay lock-free");
        std::uint32_t cap = 1;
        while (cap < capacityBlocks) {
            cap <<= 1;
        }
        m_capacity = cap;
        m_mask = cap - 1;
        m_blockIndices.assign(cap, 0);
        m_generations.assign(static_cast<std::size_t>(cap) * maxBuses, 0);
        m_peaks.assign(static_cast<std::size_t>(cap) * kMeterTapLaneCount *
                           maxBuses,
                       0.0f);
        for (std::atomic<std::uint64_t>& lane : m_overflowLanes) {
            lane.store(0, std::memory_order_relaxed);
        }
    }

    std::uint32_t Capacity() const { return m_capacity; }
    std::uint32_t MaxBuses() const { return m_maxBuses; }

    // Armed gate, owned by the logic thread (the meter wiring sets it to
    // the live pairing count); the audio thread reads it relaxed once per
    // block and skips the append entirely at zero. Ordering does not
    // matter for correctness — a block missed around arming carries no
    // pairing to feed — so relaxed is enough.
    void SetArmedCount(std::uint32_t count) {
        m_armed.store(count, std::memory_order_relaxed);
    }
    bool Armed() const {
        return m_armed.load(std::memory_order_relaxed) != 0;
    }

    // Producer side (audio thread), once per rendered block while armed. Push
    // is the Post Effects compatibility seam; PushTaps carries all three tap
    // rows. Each provided row holds busCount entries and remaining buses are
    // zero-filled.
    // Lock-free and allocation-free. On a full ring the block max-holds
    // into the overflow side-record (stamp first, then lanes — see the
    // header note) and the overflow counter bumps; published slots are
    // never touched.
    void Push(std::uint64_t blockIndex, const float* peaks,
              std::uint32_t busCount,
              const std::uint32_t* generations = nullptr) {
        PushTaps(blockIndex, nullptr, nullptr, peaks, busCount, generations);
    }

    void PushTaps(std::uint64_t blockIndex, const float* preFaderPeaks,
                  const float* postFaderPeaks,
                  const float* postEffectsPeaks, std::uint32_t busCount,
                  const std::uint32_t* generations = nullptr) {
        const std::uint32_t count =
            busCount < m_maxBuses ? busCount : m_maxBuses;
        const float* taps[kMeterTapLaneCount] = {
            postEffectsPeaks, preFaderPeaks, postFaderPeaks};
        const std::uint64_t write = m_write.load(std::memory_order_relaxed);
        const std::uint64_t read = m_read.load(std::memory_order_acquire);
        if (write - read >= m_capacity) {
            // Full: max-hold into the side-record, in the order the
            // consumer's validated take relies on — position stamp first
            // (the append position this block would have taken), then the
            // peak lanes, then the counter with a release store, so a
            // counted merge's stamp and lanes are visible to the acquire
            // reads that see its count.
            std::uint64_t stamp =
                m_overflowPosition.load(std::memory_order_relaxed);
            while (stamp < write &&
                   !m_overflowPosition.compare_exchange_weak(
                       stamp, write, std::memory_order_release,
                       std::memory_order_relaxed)) {
            }
            MergeOverflowTaps(preFaderPeaks, postFaderPeaks,
                              postEffectsPeaks, generations, count);
            m_overflow.store(m_overflow.load(std::memory_order_relaxed) + 1,
                             std::memory_order_release);
            return;
        }
        // Ordinary append: the slot at `write` is outside [read, write),
        // producer-owned — plain stores, published by the release store
        // of the write cursor below (classic SPSC happens-before).
        const std::size_t slot = RecordOffset(write);
        for (std::uint32_t lane = 0; lane < kMeterTapLaneCount; ++lane) {
            const std::size_t laneOffset = slot + LaneOffset(lane);
            const float* const tap = taps[lane];
            if (tap != nullptr) {
                for (std::uint32_t i = 0; i < count; ++i) {
                    m_peaks[laneOffset + i] = tap[i];
                }
            } else {
                for (std::uint32_t i = 0; i < count; ++i) {
                    m_peaks[laneOffset + i] = 0.0f;
                }
            }
            for (std::uint32_t i = count; i < m_maxBuses; ++i) {
                m_peaks[laneOffset + i] = 0.0f;
            }
        }
        const std::size_t generationSlot = GenerationRecordOffset(write);
        for (std::uint32_t i = 0; i < count; ++i) {
            m_generations[generationSlot + i] =
                generations != nullptr ? generations[i] : 0;
        }
        for (std::uint32_t i = count; i < m_maxBuses; ++i) {
            m_generations[generationSlot + i] = 0;
        }
        m_blockIndices[write & m_mask] = blockIndex;
        m_write.store(write + 1, std::memory_order_release);
    }

    // Consumer side (Vn_Update). Hands back a read-only view of the oldest
    // record; the caller processes it, then calls PopFront() to release the
    // slot. A published slot is never written again (the overflow path has
    // its own side-record), so the view stays plain-readable until
    // PopFront. Returns false when drained.
    bool Front(std::uint64_t* blockIndex, const float** peaks) const {
        const float* preFaderPeaks = nullptr;
        const float* postFaderPeaks = nullptr;
        return FrontTaps(blockIndex, &preFaderPeaks, &postFaderPeaks, peaks);
    }

    bool FrontTaps(std::uint64_t* blockIndex, const float** preFaderPeaks,
                   const float** postFaderPeaks,
                   const float** postEffectsPeaks,
                   const std::uint32_t** generations = nullptr) const {
        const std::uint64_t read = m_read.load(std::memory_order_relaxed);
        const std::uint64_t write = m_write.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }
        *blockIndex = m_blockIndices[read & m_mask];
        const float* row = m_peaks.data() + RecordOffset(read);
        *preFaderPeaks =
            row + LaneOffset(static_cast<std::uint32_t>(
                      MeterTapLane::PreFader));
        *postFaderPeaks =
            row + LaneOffset(static_cast<std::uint32_t>(
                      MeterTapLane::PostFader));
        *postEffectsPeaks =
            row + LaneOffset(static_cast<std::uint32_t>(
                      MeterTapLane::PostEffects));
        if (generations != nullptr) {
            *generations =
                m_generations.data() + GenerationRecordOffset(read);
        }
        return true;
    }

    void PopFront() {
        const std::uint64_t read = m_read.load(std::memory_order_relaxed);
        m_read.store(read + 1, std::memory_order_release);
    }

    // Consumer side: takes the overflow side-record's peak lanes
    // atomically, exchanging every lane to zero. Whether a compressed
    // block EXISTS is the caller's judgment from the overflow counter's
    // delta — an all-silent compressed span has all-zero lanes and must
    // still advance ballistics one step. The caller validates the take
    // against OverflowPosition() before AND after (the header's
    // validated-take note); a failed re-check merges the taken values
    // back with MergeBackOverflowLanes. outPeaks must hold maxBuses
    // entries.
    void TakeOverflowLanes(float* outPeaks) {
        TakeOverflowTaps(nullptr, nullptr, outPeaks);
    }

    void TakeOverflowTaps(float* outPreFaderPeaks,
                          float* outPostFaderPeaks,
                          float* outPostEffectsPeaks,
                          std::uint32_t* outGenerations = nullptr) {
        float* taps[kMeterTapLaneCount] = {
            outPostEffectsPeaks, outPreFaderPeaks, outPostFaderPeaks};
        for (std::uint32_t lane = 0; lane < kMeterTapLaneCount; ++lane) {
            const std::size_t laneOffset = LaneOffset(lane);
            for (std::uint32_t i = 0; i < m_maxBuses; ++i) {
                const std::uint64_t packed =
                    m_overflowLanes[laneOffset + i].exchange(
                        0, std::memory_order_acq_rel);
                const float peak = UnpackPeak(packed);
                if (taps[lane] != nullptr) {
                    taps[lane][i] = peak;
                }
                if (outGenerations != nullptr) {
                    outGenerations[laneOffset + i] =
                        UnpackGeneration(packed);
                }
            }
        }
    }

    // Consumer side, the validated take's failure path: max-merges taken
    // values back into the lanes so a deferred compressed block keeps its
    // peaks (max-fold against anything merged since — nothing lost,
    // nothing applied twice; the untaken counter delta re-triggers the
    // take later). Concurrency defense: with a single-threaded producer
    // the re-check cannot fail, so tests pin this path's max-fold
    // semantics directly rather than reaching it through drain.
    void MergeBackOverflowLanes(const float* peaks) {
        MergeOverflowLane(
            static_cast<std::uint32_t>(MeterTapLane::PostEffects), peaks,
            nullptr, m_maxBuses);
    }

    void MergeBackOverflowTaps(const float* preFaderPeaks,
                               const float* postFaderPeaks,
                               const float* postEffectsPeaks,
                               const std::uint32_t* generations = nullptr) {
        MergeOverflowTaps(preFaderPeaks, postFaderPeaks, postEffectsPeaks,
                          generations, m_maxBuses, true);
    }

    // The append position of the newest compressed block (monotonic).
    // Load-bearing for the consumer's validated take: a stamp at or
    // before the queue front means every merge recorded so far precedes
    // everything still queued.
    std::uint64_t OverflowPosition() const {
        return m_overflowPosition.load(std::memory_order_acquire);
    }

    // Monotonic append cursors. FrontPosition() is the position of the
    // record Front() currently exposes (consumer-owned, exact);
    // WriteCursor() is the position the NEXT append will take — the
    // logic thread samples it at meter-pairing time as the pairing's
    // epoch: every record already appended sits at a smaller position, so
    // "records appended from the pairing call on" is an exact cursor
    // comparison, immune to armed gaps and blockIndex reuse.
    std::uint64_t FrontPosition() const {
        return m_read.load(std::memory_order_relaxed);
    }
    std::uint64_t WriteCursor() const {
        return m_write.load(std::memory_order_acquire);
    }

    // Blocks compressed into the overflow side-record since construction.
    // Producer-owned counter; the consumer's acquire read pairs with the
    // producer's release store, so a counted merge's stamp and lanes are
    // visible to the take that acts on its count.
    std::uint64_t OverflowCount() const {
        return m_overflow.load(std::memory_order_acquire);
    }

private:
    std::size_t LaneOffset(std::uint32_t lane) const {
        return MeterTapOffset(lane, m_maxBuses);
    }

    std::size_t RecordOffset(std::uint64_t position) const {
        return static_cast<std::size_t>(position & m_mask) *
               kMeterTapLaneCount * m_maxBuses;
    }

    std::size_t GenerationRecordOffset(std::uint64_t position) const {
        return static_cast<std::size_t>(position & m_mask) * m_maxBuses;
    }

    static std::uint64_t PackLane(std::uint32_t generation, float peak) {
        std::uint32_t peakBits = 0;
        std::memcpy(&peakBits, &peak, sizeof(peakBits));
        return (static_cast<std::uint64_t>(generation) << 32) | peakBits;
    }

    static std::uint32_t UnpackGeneration(std::uint64_t packed) {
        return static_cast<std::uint32_t>(packed >> 32);
    }

    static float UnpackPeak(std::uint64_t packed) {
        const std::uint32_t peakBits =
            static_cast<std::uint32_t>(packed);
        float peak = 0.0f;
        std::memcpy(&peak, &peakBits, sizeof(peak));
        return peak;
    }

    // Fold the newest generation and max-hold within that generation. This is
    // shared by the producer's overflow merge and the consumer's take-failure
    // merge-back, so an older taken lane cannot replace a concurrently
    // published newer Bus generation.
    void MergeOverflowLane(std::uint32_t tap, const float* peaks,
                           const std::uint32_t* generations,
                           std::uint32_t count) {
        if (peaks == nullptr) {
            return;
        }
        const std::size_t laneOffset = LaneOffset(tap);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::atomic<std::uint64_t>& destination =
                m_overflowLanes[laneOffset + i];
            const std::uint32_t generation =
                generations != nullptr ? generations[i] : 0;
            std::uint64_t lane =
                destination.load(std::memory_order_relaxed);
            for (;;) {
                const std::uint32_t laneGeneration =
                    UnpackGeneration(lane);
                const float lanePeak = UnpackPeak(lane);
                if (laneGeneration > generation ||
                    (laneGeneration == generation &&
                     !(peaks[i] > lanePeak))) {
                    break;
                }
                const std::uint64_t replacement =
                    PackLane(generation, peaks[i]);
                if (destination.compare_exchange_weak(
                        lane, replacement, std::memory_order_release,
                        std::memory_order_relaxed)) {
                    break;
                }
            }
        }
    }

    void MergeOverflowTaps(const float* preFaderPeaks,
                           const float* postFaderPeaks,
                           const float* postEffectsPeaks,
                           const std::uint32_t* generations,
                           std::uint32_t count,
                           bool generationsPerTap = false) {
        const auto generationRow = [&](std::uint32_t tap) {
            return generations != nullptr && generationsPerTap
                       ? generations + MeterTapOffset(tap, m_maxBuses)
                       : generations;
        };
        MergeOverflowLane(
            static_cast<std::uint32_t>(MeterTapLane::PreFader),
            preFaderPeaks,
            generationRow(static_cast<std::uint32_t>(
                MeterTapLane::PreFader)),
            count);
        MergeOverflowLane(
            static_cast<std::uint32_t>(MeterTapLane::PostFader),
            postFaderPeaks,
            generationRow(static_cast<std::uint32_t>(
                MeterTapLane::PostFader)),
            count);
        MergeOverflowLane(
            static_cast<std::uint32_t>(MeterTapLane::PostEffects),
            postEffectsPeaks,
            generationRow(static_cast<std::uint32_t>(
                MeterTapLane::PostEffects)),
            count);
    }

    std::uint32_t m_capacity = 0;
    std::uint32_t m_mask = 0;
    std::uint32_t m_maxBuses = 0;
    std::vector<std::uint64_t> m_blockIndices; // fixed at construction
    std::vector<std::uint32_t> m_generations; // capacity x maxBuses rows
    std::vector<float> m_peaks; // capacity x tap lanes x maxBuses rows
    std::atomic<std::uint64_t> m_write{0}; // producer-owned
    std::atomic<std::uint64_t> m_read{0};  // consumer-owned
    std::atomic<std::uint64_t> m_overflow{0};
    std::atomic<std::uint32_t> m_armed{0};
    // Overflow side-record: one atomic generation+peak lane per tap and bus,
    // plus the monotonic position stamp of the newest compressed block (the
    // validated take's ordering guard — see the header note).
    std::vector<std::atomic<std::uint64_t>> m_overflowLanes;
    std::atomic<std::uint64_t> m_overflowPosition{0};
};

} // namespace harken

#endif // HARKEN_CORE_METER_BLOCK_RING_H
