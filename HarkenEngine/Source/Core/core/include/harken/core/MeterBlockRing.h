// SPSC block peaks preserve ordinary sample order and subscription epochs.
// Audio writes only lanes leased by its immutable Bus activity plan. Sleeping
// sources have implicit zero samples, so release ballistics still advance.
// Logic owns all allocation and releases a lane only after its last activity
// lease, ordinary records and compressed overflow records have retired.
//
// A full ring never modifies published ordinary samples. It stamps the write
// position, CAS-max merges atomic generation/peak lanes, then publishes an
// overflow counter. The consumer exchanges every retained lane (including
// sleeping sources with unread history), rechecks the stamp, and either uses
// one compressed step or merges the whole take back. A silent compressed span
// still advances release once; counter-lag splits are release-conservative.
// Its watermark advances only to the counter read before the take, ensuring
// a racing merge cannot strand a peak behind an acknowledged count.
//
// Overflow is intentionally time-compressed and may attribute an older peak
// to a new subscription for one compressed step. Ordinary records retain the
// exact per-probe write-cursor epoch even when installation overlaps capture.
// No allocation, shared ownership mutation, locks or I/O run on audio.
#ifndef HARKEN_CORE_METER_BLOCK_RING_H
#define HARKEN_CORE_METER_BLOCK_RING_H

#include <harken/Harken.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <map>
#include <memory>
#include <span>

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
    struct Sample {
        std::uint64_t position = UINT64_MAX;
        std::uint32_t generation = 0;
        std::array<float, kMeterTapLaneCount> peaks{};
    };
    // Logic owns the allocation. Immutable activity plans hold leases while
    // audio can write it; queued samples retain it after the last plan retires.
    struct Lane {
        Lane(std::uint32_t index, std::uint32_t capacity) : bus(index), samples(capacity) {}
        const std::uint32_t bus;
        std::vector<Sample> samples;
        std::array<std::atomic<std::uint64_t>, kMeterTapLaneCount> overflow{};
        std::array<std::uint64_t, kMeterTapLaneCount> taken{}; // sole consumer
        std::uint64_t ordinaryEnd = 0, overflowEnd = 0; // audio, read after final lease
    };
    using Lease = std::shared_ptr<Lane>;

    // Direct producers can prepare a fixed directory at construction. Engine
    // production prepares exact lanes with its immutable Bus activity plans.
    MeterBlockRing(std::uint32_t capacityBlocks, std::uint32_t maxBuses,
                   bool prepareDirectory = true) : m_maxBuses(maxBuses) {
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        m_capacity = 1;
        while (m_capacity < capacityBlocks) m_capacity <<= 1;
        m_mask = m_capacity - 1;
        m_blockIndices.resize(m_capacity);
        if (prepareDirectory) {
            m_direct.reserve(maxBuses);
            for (std::uint32_t bus = 0; bus < maxBuses; ++bus)
                m_direct.push_back(PrepareLane(bus));
        }
    }
    Lease PrepareLane(std::uint32_t bus) {
        const auto found = m_lanes.find(bus);
        if (found != m_lanes.end()) return found->second;
        auto lane = std::make_shared<Lane>(bus, m_capacity);
        m_lanes.emplace(bus, lane);
        return lane;
    }
    std::uint32_t Capacity() const { return m_capacity; }
    std::uint32_t MaxBuses() const { return m_maxBuses; }
    std::size_t LaneCount() const noexcept { return m_lanes.size(); }
    std::uint64_t StorageBytes() const noexcept {
        std::uint64_t bytes = m_blockIndices.capacity() * sizeof(std::uint64_t) +
            m_direct.capacity() * sizeof(Lease) + m_viewPeaks.capacity() * sizeof(float) +
            m_viewGenerations.capacity() * sizeof(std::uint32_t);
        for (const auto& [bus, lane] : m_lanes)
            bytes += sizeof(Lane) + lane->samples.capacity() * sizeof(Sample) +
                sizeof(std::pair<const std::uint32_t, Lease>) + 3 * sizeof(void*);
        return bytes;
    }
    void SetArmedCount(std::uint32_t count) { m_armed.store(count, std::memory_order_relaxed); }
    bool Armed() const { return m_armed.load(std::memory_order_relaxed) != 0; }

    template<class Capture>
    void PushCaptured(std::uint64_t blockIndex, std::span<const Lease> lanes, Capture&& capture) {
        const auto write = m_write.load(std::memory_order_relaxed);
        const bool full = write - m_read.load(std::memory_order_acquire) >= m_capacity;
        const auto overflowEnd = m_overflow.load(std::memory_order_relaxed) + 1;
        if (full) {
            auto stamp = m_overflowPosition.load(std::memory_order_relaxed);
            while (stamp < write && !m_overflowPosition.compare_exchange_weak(
                stamp, write, std::memory_order_release, std::memory_order_relaxed)) {}
        }
        for (const auto& lease : lanes) {
            auto& lane = *lease;
            Sample sample;
            sample.position = write;
            capture(lane.bus, sample.generation, sample.peaks);
            if (full) {
                for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap)
                    Merge(lane.overflow[tap], PackLane(sample.generation, sample.peaks[tap]));
                lane.overflowEnd = overflowEnd;
            } else {
                lane.samples[write & m_mask] = sample;
                lane.ordinaryEnd = write + 1;
            }
        }
        if (full) m_overflow.store(overflowEnd, std::memory_order_release);
        else {
            m_blockIndices[write & m_mask] = blockIndex;
            m_write.store(write + 1, std::memory_order_release);
        }
    }
    void Push(std::uint64_t blockIndex, const float* peaks, std::uint32_t busCount,
              const std::uint32_t* generations = nullptr) {
        PushTaps(blockIndex, nullptr, nullptr, peaks, busCount, generations);
    }
    void PushTaps(std::uint64_t blockIndex, const float* pre, const float* post,
                  const float* effects, std::uint32_t busCount,
                  const std::uint32_t* generations = nullptr) {
        const float* taps[] = {effects, pre, post};
        PushCaptured(blockIndex, m_direct, [&](auto bus, auto& generation, auto& peaks) {
            generation = bus < busCount && generations ? generations[bus] : 0;
            for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap)
                peaks[tap] = bus < busCount && taps[tap] ? taps[tap][bus] : 0;
        });
    }
    bool FrontBlock(std::uint64_t* blockIndex) const {
        const auto read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return false;
        *blockIndex = m_blockIndices[read & m_mask];
        return true;
    }
    // Absent capture is a cold, silent source. A present sample from a
    // different incarnation must never drive a new probe's envelope.
    bool FrontSample(std::uint32_t bus, std::uint32_t generation,
                     std::uint32_t tap, float& peak) const noexcept {
        peak = 0;
        const auto* sample = SampleAt(bus, FrontPosition());
        if (!sample) return true;
        if (sample->generation != generation) return false;
        peak = sample->peaks[tap];
        return true;
    }
    bool Front(std::uint64_t* blockIndex, const float** peaks) const {
        const float *pre = nullptr, *post = nullptr;
        return FrontTaps(blockIndex, &pre, &post, peaks);
    }
    // Contiguous compatibility view is built by the consumer only on demand;
    // production drains individual probe sources without a dense scratch row.
    bool FrontTaps(std::uint64_t* blockIndex, const float** pre, const float** post,
                   const float** effects, const std::uint32_t** generations = nullptr) const {
        if (!FrontBlock(blockIndex)) return false;
        m_viewPeaks.assign(static_cast<std::size_t>(m_maxBuses) * kMeterTapLaneCount, 0);
        m_viewGenerations.assign(m_maxBuses, 0);
        for (std::uint32_t bus = 0; bus < m_maxBuses; ++bus) {
            if (const auto* sample = SampleAt(bus, FrontPosition())) {
                m_viewGenerations[bus] = sample->generation;
                for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap)
                    m_viewPeaks[MeterTapOffset(tap, m_maxBuses) + bus] = sample->peaks[tap];
            }
        }
        *effects = m_viewPeaks.data();
        *pre = m_viewPeaks.data() + MeterTapOffset(1, m_maxBuses);
        *post = m_viewPeaks.data() + MeterTapOffset(2, m_maxBuses);
        if (generations) *generations = m_viewGenerations.data();
        return true;
    }
    void PopFront() { m_read.store(FrontPosition() + 1, std::memory_order_release); }
    void TakeOverflowSnapshot() noexcept {
        for (auto& [bus, lane] : m_lanes)
            for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap)
                lane->taken[tap] = lane->overflow[tap].exchange(0, std::memory_order_acq_rel);
    }
    void MergeBackOverflowSnapshot() noexcept {
        for (auto& [bus, lane] : m_lanes)
            for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap)
                Merge(lane->overflow[tap], lane->taken[tap]);
    }
    bool OverflowSample(std::uint32_t bus, std::uint32_t generation,
                        std::uint32_t tap, float& peak) const noexcept {
        peak = 0;
        const auto found = m_lanes.find(bus);
        if (found == m_lanes.end()) return true;
        const auto packed = found->second->taken[tap];
        if (packed == 0) return true;
        if (UnpackGeneration(packed) != generation) return false;
        peak = UnpackPeak(packed);
        return true;
    }
    void AcknowledgeOverflow(std::uint64_t counted) noexcept { m_consumedOverflow = counted; }
    void ReclaimConsumed() noexcept {
        for (auto it = m_lanes.begin(); it != m_lanes.end();) {
            // A last activity release is observed by the logic owner through
            // the existing audio-plan retirement protocol before this point.
            const auto& lane = it->second;
            if (lane.use_count() == 1 && lane->ordinaryEnd <= FrontPosition() &&
                lane->overflowEnd <= m_consumedOverflow) it = m_lanes.erase(it);
            else ++it;
        }
    }
    void TakeOverflowLanes(float* peaks) { TakeOverflowTaps(nullptr, nullptr, peaks); }
    void TakeOverflowTaps(float* pre, float* post, float* effects,
                          std::uint32_t* generations = nullptr) {
        TakeOverflowSnapshot();
        float* taps[] = {effects, pre, post};
        for (std::uint32_t bus = 0; bus < m_maxBuses; ++bus) {
            const auto found = m_lanes.find(bus);
            for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap) {
                const auto packed = found == m_lanes.end() ? 0 : found->second->taken[tap];
                if (taps[tap]) taps[tap][bus] = UnpackPeak(packed);
                if (generations) generations[MeterTapOffset(tap, m_maxBuses) + bus] = UnpackGeneration(packed);
            }
        }
    }
    void MergeBackOverflowLanes(const float* peaks) {
        MergeBackOverflowTaps(nullptr, nullptr, peaks);
    }
    void MergeBackOverflowTaps(const float* pre, const float* post, const float* effects,
                               const std::uint32_t* generations = nullptr) {
        const float* taps[] = {effects, pre, post};
        for (auto& [bus, lane] : m_lanes) {
            if (bus >= m_maxBuses) continue;
            for (std::uint32_t tap = 0; tap < kMeterTapLaneCount; ++tap)
                if (taps[tap]) Merge(lane->overflow[tap], PackLane(generations
                    ? generations[MeterTapOffset(tap, m_maxBuses) + bus] : 0, taps[tap][bus]));
        }
    }
    std::uint64_t OverflowPosition() const { return m_overflowPosition.load(std::memory_order_acquire); }
    std::uint64_t FrontPosition() const { return m_read.load(std::memory_order_relaxed); }
    std::uint64_t WriteCursor() const { return m_write.load(std::memory_order_acquire); }
    std::uint64_t OverflowCount() const { return m_overflow.load(std::memory_order_acquire); }
private:
    const Sample* SampleAt(std::uint32_t bus, std::uint64_t position) const noexcept {
        const auto found = m_lanes.find(bus);
        if (found == m_lanes.end()) return nullptr;
        const auto& sample = found->second->samples[position & m_mask];
        return sample.position == position ? &sample : nullptr;
    }
    static std::uint64_t PackLane(std::uint32_t generation, float peak) {
        std::uint32_t bits;
        std::memcpy(&bits, &peak, sizeof(bits));
        return (static_cast<std::uint64_t>(generation) << 32) | bits;
    }
    static std::uint32_t UnpackGeneration(std::uint64_t packed) { return static_cast<std::uint32_t>(packed >> 32); }
    static float UnpackPeak(std::uint64_t packed) {
        const auto bits = static_cast<std::uint32_t>(packed);
        float peak;
        std::memcpy(&peak, &bits, sizeof(peak));
        return peak;
    }
    static void Merge(std::atomic<std::uint64_t>& destination, std::uint64_t incoming) noexcept {
        auto old = destination.load(std::memory_order_relaxed);
        for (;;) {
            if (UnpackGeneration(old) > UnpackGeneration(incoming) ||
                (UnpackGeneration(old) == UnpackGeneration(incoming) &&
                 !(UnpackPeak(incoming) > UnpackPeak(old)))) return;
            if (destination.compare_exchange_weak(old, incoming,
                std::memory_order_release, std::memory_order_relaxed)) return;
        }
    }
    std::uint32_t m_capacity = 0, m_mask = 0, m_maxBuses = 0;
    std::vector<std::uint64_t> m_blockIndices;
    std::map<std::uint32_t, Lease> m_lanes; // logic only; audio uses immutable leases
    std::vector<Lease> m_direct;
    mutable std::vector<float> m_viewPeaks;
    mutable std::vector<std::uint32_t> m_viewGenerations;
    std::atomic<std::uint64_t> m_write{0}, m_read{0}, m_overflow{0};
    std::atomic<std::uint64_t> m_overflowPosition{0};
    std::atomic<std::uint32_t> m_armed{0};
    std::uint64_t m_consumedOverflow = 0;
};

} // namespace harken
#endif // HARKEN_CORE_METER_BLOCK_RING_H
