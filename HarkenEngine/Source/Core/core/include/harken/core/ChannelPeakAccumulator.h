#ifndef HARKEN_CORE_CHANNEL_PEAK_ACCUMULATOR_H
#define HARKEN_CORE_CHANNEL_PEAK_ACCUMULATOR_H

#include <harken/Harken.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

namespace harken {

struct ChannelPeakBatch {
    std::uint64_t consumeSerial = 0;
    std::uint32_t mixChannelCount = 0;
    std::uint32_t busCount = 0;
    std::array<VnBusId, VN_SNAPSHOT_MAX_BUSES> busIds{};
    std::array<float, VN_SNAPSHOT_MAX_BUSES * VN_MAX_CHANNELS> channelPeaks{};
};

// One audio producer and one logic consumer. Each lane changes windows
// independently; these maxima do not describe a single rendered block.
class ChannelPeakAccumulator {
public:
    void Publish(std::uint32_t bus, std::uint32_t generation,
                 std::uint32_t channel, float peak) noexcept {
        if (bus >= VN_SNAPSHOT_MAX_BUSES || channel >= VN_MAX_CHANNELS || generation == 0)
            return;
        if (!(peak >= 0.0f) || !std::isfinite(peak))
            peak = 0.0f;
        const auto value = (static_cast<std::uint64_t>(generation) << 32)
            | std::bit_cast<std::uint32_t>(peak);
        auto& lane = m_lanes[bus * VN_MAX_CHANNELS + channel];
        auto observed = lane.load(std::memory_order_relaxed);
        for (;;) {
            if (static_cast<std::uint32_t>(observed >> 32) == generation
                && std::bit_cast<float>(static_cast<std::uint32_t>(observed)) >= peak)
                return;
            if (lane.compare_exchange_weak(observed, value,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return;
        }
    }

    bool Take(std::uint32_t bus, std::uint32_t generation,
              std::uint32_t channel, float& peak) noexcept {
        const auto value = m_lanes[bus * VN_MAX_CHANNELS + channel].exchange(
            0, std::memory_order_relaxed);
        if (generation == 0 || static_cast<std::uint32_t>(value >> 32) != generation)
            return false;
        peak = std::bit_cast<float>(static_cast<std::uint32_t>(value));
        return true;
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::array<std::atomic<std::uint64_t>, VN_SNAPSHOT_MAX_BUSES * VN_MAX_CHANNELS> m_lanes{};
};

} // namespace harken

#endif // HARKEN_CORE_CHANNEL_PEAK_ACCUMULATOR_H
