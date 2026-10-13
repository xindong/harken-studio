// Source-timeline mirror: the audio thread publishes, once per block, each
// configured voice slot's render generation, source cursor, and remaining loop
// passes so the logic thread has coherent authority for audibility parking.
// One row per configured voice slot — deliberately NOT the public snapshot's
// voices[] array
// (capped at 64) — because parking must work at any pool size.
//
// This is a dedicated internal three-slot mirror, separate from the snapshot
// payload: the tuple is engine-internal timeline authority, not a public
// observability field, and its width is maxVoices rather than the frozen 64-row
// public layout. A single-reader pin protects the confirmed published slot while
// it is copied. Storage is preallocated at construction; BeginWrite/EndWrite
// touch no heap, take no locks, and never wait.
#ifndef HARKEN_CORE_SOURCE_CURSOR_MIRROR_H
#define HARKEN_CORE_SOURCE_CURSOR_MIRROR_H

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace harken {

class SourceCursorMirror {
public:
    // One configured voice slot's published timeline authority for a block.
    struct Row {
        std::uint64_t sourceFrame = 0;          // frames pushed to the resampler
        std::uint32_t renderGeneration = 0;     // generation occupying this slot
        std::uint32_t loopPassesRemaining = 0;  // 0 for infinite or inactive
        bool active = false;                    // rendered in this slot this block
    };
    static_assert(std::is_trivially_copyable_v<Row>,
                  "cursor rows must remain plain copyable payload");

    // slotCount = configured voice-pool size (maxVoices). All three slots hold
    // that many rows, allocated once here.
    explicit SourceCursorMirror(std::uint32_t slotCount)
        : m_slotCount(slotCount == 0 ? 1 : slotCount) {
        m_slots[0].rows.assign(m_slotCount, Row{});
        m_slots[1].rows.assign(m_slotCount, Row{});
        m_slots[2].rows.assign(m_slotCount, Row{});
    }

    std::uint32_t SlotCount() const { return m_slotCount; }

    // Writer side (audio thread). Every row must be written each block because
    // a slot retains its payload between uses.
    Row* BeginWrite() {
        const std::uint32_t pin = m_pin.load(std::memory_order_seq_cst);
        m_writeIndex = (m_lastPublished + 1u) % kSlotCount;
        if (m_writeIndex == pin) {
            m_writeIndex = (m_writeIndex + 1u) % kSlotCount;
        }
        return m_slots[m_writeIndex].rows.data();
    }

    // Publishes the filled half stamped with the block index it describes.
    void EndWrite(std::uint64_t blockIndex) {
        Slot& slot = m_slots[m_writeIndex];
        slot.blockIndex = blockIndex;
        m_published.store(m_writeIndex, std::memory_order_seq_cst);
        m_lastPublished = m_writeIndex;
    }

    // Reader side (logic thread). The confirmed pin protects the rows and block
    // index as one publication. On collision, both caller outputs stay intact.
    bool Capture(Row* out, std::uint64_t* outBlockIndex) const {
        for (int attempt = 0; attempt < kCaptureAttempts; ++attempt) {
            const std::uint32_t candidate =
                m_published.load(std::memory_order_seq_cst);
            m_pin.store(candidate, std::memory_order_seq_cst);
            if (m_published.load(std::memory_order_seq_cst) != candidate) {
                m_pin.store(kNoPin, std::memory_order_release);
                continue;
            }
            const Slot& slot = m_slots[candidate];
            for (std::uint32_t i = 0; i < m_slotCount; ++i) {
                out[i] = slot.rows[i];
            }
            const std::uint64_t blockIndex = slot.blockIndex;
            m_pin.store(kNoPin, std::memory_order_release);
            *outBlockIndex = blockIndex;
            return true;
        }
        m_pin.store(kNoPin, std::memory_order_release);
        return false;
    }

private:
    static constexpr int kCaptureAttempts = 4;
    static constexpr std::uint32_t kSlotCount = 3;
    static constexpr std::uint32_t kNoPin = kSlotCount;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "control words must be lock-free on the audio thread");
    static_assert(kSlotCount == 3,
                  "single-reader ownership requires three slots");
    static_assert(kNoPin == kSlotCount,
                  "the no-pin value must immediately follow the slots");

    struct Slot {
        std::vector<Row> rows;
        std::uint64_t blockIndex = 0;
    };

    std::uint32_t m_slotCount;
    Slot m_slots[kSlotCount];
    std::atomic<std::uint32_t> m_published{0};
    mutable std::atomic<std::uint32_t> m_pin{kNoPin};
    std::uint32_t m_lastPublished = 0; // writer-owned
    std::uint32_t m_writeIndex = 1;    // writer-owned selected slot
};

} // namespace harken

#endif // HARKEN_CORE_SOURCE_CURSOR_MIRROR_H
