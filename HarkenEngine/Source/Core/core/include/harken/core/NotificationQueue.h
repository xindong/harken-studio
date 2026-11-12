// Reverse transport (audio -> logic): a single-producer single-consumer ring
// of POD notifications (VnNotification). The producer is the audio thread; the
// consumer is Vn_Update, which drains the ring and invokes the user callback
// on the pumping thread.
//
// Full-queue policy: the oldest notifications are dropped and counted —
// notifications are advisory, never load-bearing for correctness, so the
// newest information wins. The drop is realized as an overwrite ring: the
// producer always writes and advances; the consumer detects overrun by index
// lag, skips the overwritten span, and accumulates the drop counter. Neither
// side ever touches the other's index. Push is wait-free; TryPop performs
// bounded work per call and never blocks the producer.
//
// The producer (a real audio-callback thread once the device backend is live)
// can lap the consumer while a slot is being copied, so the consumer must
// never deliver a slot the producer may still be reclaiming. The producer's
// current write reclaims the slot at lag == capacity, so the survivor window
// is capacity - 1: on overrun the consumer resyncs to (write - capacity + 1),
// conservatively dropping and counting that boundary record. After copying a
// surviving slot the consumer re-reads the write cursor; if the post-copy lag
// reached capacity the slot may have been overwritten mid-read, so the copy is
// discarded and retried within a four-copy budget. On budget exhaustion the
// consumer publishes its resync progress and yields until the next TryPop
// call. Only a copy proven untouched (post-copy lag < capacity) is delivered,
// so a torn record never surfaces.
//
// Slot storage is per-word atomic: each 24-byte record is three
// std::atomic<uint64_t> words. A producer store and a consumer load of the
// same slot are therefore conflicting atomic accesses, not a data race — the
// access is well-defined by the memory model and clean under a race detector
// by construction. Each word uses release/acquire ordering. A cross-word tear
// necessarily includes a word from a later overwrite, whose acquire load
// synchronizes with the producer's release store. That overwrite is sequenced
// after the release of the write cursor that made the slot reclaimable, so the
// resulting happens-before chain forces the post-copy cursor acquire to observe
// that cursor value or a later one. Its lag therefore reaches capacity and the
// mixed copy is rejected.
//
// Storage is allocated once at engine init; push and pop touch no heap.
#ifndef HARKEN_CORE_NOTIFICATION_QUEUE_H
#define HARKEN_CORE_NOTIFICATION_QUEUE_H

#include <harken/Harken.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace harken {

static_assert(std::is_trivially_copyable_v<VnNotification>,
              "notifications cross threads by memcpy semantics");
static_assert(sizeof(VnNotification) == 24,
              "notification record layout is frozen");
static_assert(sizeof(VnNotification) == sizeof(std::uint64_t[3]),
              "the record maps onto exactly three slot words");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "slot words must be lock-free for a wait-free audio-thread push");

class NotificationQueue {
public:
    // Depth rounds up to a power of two and clamps to at least two. A one-slot
    // ring has an empty capacity - 1 survivor window, so it cannot deliver.
    explicit NotificationQueue(std::uint32_t depth) {
        std::uint32_t cap = 2;
        while (cap < depth) {
            cap <<= 1;
        }
        m_slots = std::make_unique<Slot[]>(cap);
        m_capacity = cap;
        m_mask = cap - 1;
    }

    std::uint32_t Capacity() const { return m_capacity; }

    // Opts this queue into the per-quantum producer budget. Off by default so a
    // queue nobody drives from an audio block is never judged against a quantum
    // it has none of; the block's owner turns it on at construction.
    void EnableQuantumBudget() { m_quantumBudgetEnabled = true; }

    // Opens a quantum: clears only what this block has produced. The
    // budget-drop total and the produced high-water outlive the block, because
    // both are read after the fact.
    void BeginQuantum() { m_producedThisQuantum = 0; }

    // Producer side (audio thread). When the ring is full the oldest unread
    // record is overwritten, as before.
    //
    // Returns false only when the per-quantum budget refused the record. The
    // budget is the resolved capacity, not the requested depth: capacity is what
    // the queue can actually hold, so a block producing more than that has
    // already overwritten its own earlier events. Refusal is a drop -- a voice
    // event describes a moment in this block and cannot be moved to another one.
    //
    // Existing callers ignore the result and must keep ignoring it: whether a
    // notification survived may not change what the block renders. The return
    // exists for tests and accounting.
    bool Push(const VnNotification& notification) {
        if (m_quantumBudgetEnabled) {
            if (m_producedThisQuantum >= m_capacity) {
                ++m_quantumBudgetDropped;
                return false;
            }
            ++m_producedThisQuantum;
            if (m_producedThisQuantum > m_producedHighWater) {
                m_producedHighWater = m_producedThisQuantum;
            }
        }
        PushUnbudgeted(notification);
        return true;
    }

    // Notifications refused because one quantum had already produced the
    // queue's capacity. Kept apart from DroppedCount(), which counts records
    // overwritten before the consumer read them: the first says a block
    // produced too much, the second says the consumer drained too late.
    std::uint64_t QuantumBudgetDropCount() const {
        return m_quantumBudgetDropped;
    }

    // The most notifications any one quantum has produced, refusals excluded.
    // Not the same as occupancy, which also depends on when the consumer ran.
    std::uint32_t QuantumProducedHighWater() const {
        return m_producedHighWater;
    }

private:
    void PushUnbudgeted(const VnNotification& notification) {
        std::uint64_t words[3];
        std::memcpy(words, &notification, sizeof words);
        const std::uint64_t write = m_write.load(std::memory_order_relaxed);
        Slot& slot = m_slots[write & m_mask];
        slot.words[0].store(words[0], std::memory_order_release);
        slot.words[1].store(words[1], std::memory_order_release);
        slot.words[2].store(words[2], std::memory_order_release);
        m_write.store(write + 1, std::memory_order_release);
    }

public:
    // Consumer side (Vn_Update). Skips any overwritten span first, adding its
    // length to the drop counter, then reads the oldest surviving record and
    // revalidates the copy against the write cursor before delivering it.
    // Returns false when drained or when the bounded copy budget is exhausted.
    bool TryPop(VnNotification& out) {
        // Four attempts absorb transient producer laps while keeping each pump
        // call's consumer work small and fixed.
        constexpr std::uint32_t kMaxInvalidatedCopies = 4;
        const std::uint64_t capacity = m_capacity;
        std::uint64_t read = m_read.load(std::memory_order_relaxed);
        std::uint64_t write = m_write.load(std::memory_order_acquire);
        // How full the queue was when this drain reached it, saturated at
        // capacity because a lap makes the raw lag exceed what the ring holds.
        // Measured on the consumer, where both indices are readable, so the
        // audio-thread producers are untouched. The drop counter reports
        // overflow after the fact; this reports the headroom that was left, and
        // only the latter can size a queue depth.
        const std::uint64_t lag = write - read;
        const std::uint32_t occupancy =
            lag >= capacity ? m_capacity : static_cast<std::uint32_t>(lag);
        if (occupancy > m_occupancyHighWater) {
            m_occupancyHighWater = occupancy;
        }
        std::uint32_t invalidatedCopies = 0;
        for (;;) {
            if (read == write) {
                return false;
            }
            if (write - read >= capacity) {
                const std::uint64_t resync = write - capacity + 1;
                m_dropped += resync - read;
                read = resync;
                // Resync can reach the observed write cursor; recheck the
                // empty state before copying a slot.
                continue;
            }
            Slot& slot = m_slots[read & m_mask];
            std::uint64_t words[3];
            words[0] = slot.words[0].load(std::memory_order_acquire);
            words[1] = slot.words[1].load(std::memory_order_acquire);
            words[2] = slot.words[2].load(std::memory_order_acquire);
            std::memcpy(&out, words, sizeof words);
            // Revalidation: if the lag reached capacity while the slot was
            // being copied, the producer may have reclaimed it mid-copy.
            const std::uint64_t writeAfter =
                m_write.load(std::memory_order_acquire);
            if (writeAfter - read < capacity) {
                m_read.store(read + 1, std::memory_order_release);
                return true;
            }
            write = writeAfter;
            if (++invalidatedCopies >= kMaxInvalidatedCopies) {
                // Preserve resync progress before yielding so the next call
                // does not count an already skipped span as dropped again.
                m_read.store(read, std::memory_order_release);
                return false;
            }
        }
    }

    // Total notifications lost to the drop-oldest policy, accumulated at drain
    // time. Internal observability; snapshot exposure is a later surface.
    std::uint64_t DroppedCount() const { return m_dropped; }

    // The fullest this queue has been observed at a drain, in records,
    // saturated at capacity. Consumer-side state like the drop counter. Zero
    // for a session whose drains always found it empty.
    std::uint32_t OccupancyHighWater() const { return m_occupancyHighWater; }

private:
    // One record as three lock-free words. std::atomic is not movable, so the
    // ring cannot live in a resizable container; it is a single fixed
    // allocation whose capacity is held separately.
    struct Slot {
        std::atomic<std::uint64_t> words[3];
    };
    static_assert(sizeof(Slot) == sizeof(VnNotification),
                  "slot storage must match the notification record size");
    static_assert(alignof(Slot) == alignof(std::uint64_t),
                  "slot storage must retain uint64_t alignment");

    std::unique_ptr<Slot[]> m_slots;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_mask = 0;
    std::atomic<std::uint64_t> m_write{0}; // producer-owned
    std::atomic<std::uint64_t> m_read{0};  // consumer-owned
    std::uint64_t m_dropped = 0;           // consumer-side accumulation
    std::uint32_t m_occupancyHighWater = 0; // consumer-side accumulation
    // Producer-side quantum budget state, written only by the pushing thread
    // and read by the logic thread through the snapshot -- the same shape as the
    // mixer's already-published per-block deferral counters.
    bool m_quantumBudgetEnabled = false;
    std::uint32_t m_producedThisQuantum = 0;
    std::uint32_t m_producedHighWater = 0;
    std::uint64_t m_quantumBudgetDropped = 0;
};

} // namespace harken

#endif // HARKEN_CORE_NOTIFICATION_QUEUE_H
