#ifndef HARKEN_API_MONITOR_PRODUCER_RING_H
#define HARKEN_API_MONITOR_PRODUCER_RING_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace harken {
namespace monitor {

// Single-producer single-consumer ring of preallocated byte slots. The
// producer (audio-tagged test/publish path) never allocates, locks, or
// waits. A full ring refuses the push so the caller can count a drop.
class MonitorProducerRing {
public:
    MonitorProducerRing() = default;

    void Init(std::uint32_t slotCount, std::uint32_t slotBytes) {
        std::uint32_t cap = 1;
        while (cap < slotCount) {
            cap <<= 1;
        }
        slotBytes_ = slotBytes;
        mask_ = cap - 1;
        storage_.assign(static_cast<std::size_t>(cap) * slotBytes, 0);
        lengths_.assign(cap, 0);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    void Reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        if (!lengths_.empty()) {
            std::fill(lengths_.begin(), lengths_.end(), 0);
        }
    }

    void Clear() {
        std::vector<std::uint8_t>().swap(storage_);
        std::vector<std::uint32_t>().swap(lengths_);
        slotBytes_ = 0;
        mask_ = 0;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    std::uint32_t SlotBytes() const { return slotBytes_; }
    std::uint32_t Capacity() const {
        return static_cast<std::uint32_t>(lengths_.size());
    }

    std::uint32_t FreeSlots() const {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint32_t cap = Capacity();
        if (cap == 0 || head <= tail) {
            return cap;
        }
        const std::uint64_t used = head - tail;
        return used >= cap ? 0 : cap - static_cast<std::uint32_t>(used);
    }

    bool Empty() const {
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        return tail >= head;
    }

    bool TryPush(const std::uint8_t* bytes, std::uint32_t size) {
        if (bytes == nullptr || size == 0 || size > slotBytes_ ||
            lengths_.empty()) {
            return false;
        }
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= lengths_.size()) {
            return false;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(head & mask_);
        std::uint8_t* slot = storage_.data() +
                             static_cast<std::size_t>(index) * slotBytes_;
        std::memcpy(slot, bytes, size);
        lengths_[index] = size;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(std::uint8_t* out, std::uint32_t capacity, std::uint32_t* size) {
        if (out == nullptr || size == nullptr || lengths_.empty()) {
            return false;
        }
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail >= head) {
            return false;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(tail & mask_);
        const std::uint32_t bytes = lengths_[index];
        if (bytes > capacity) {
            return false;
        }
        const std::uint8_t* slot = storage_.data() +
                                   static_cast<std::size_t>(index) * slotBytes_;
        std::memcpy(out, slot, bytes);
        *size = bytes;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    std::vector<std::uint8_t> storage_{};
    std::vector<std::uint32_t> lengths_{};
    std::uint32_t slotBytes_ = 0;
    std::uint32_t mask_ = 0;
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
};

} // namespace monitor
} // namespace harken

#endif
