// Audio-owned source progress, shared by parking and source-timed controls.
// One serialized logic owner prepares stable rows before publishing voice slots.
// The audio writer never allocates, waits, or clears unused rows. A single-reader
// pin protects each publication; its serial distinguishes untouched ring rows.
#ifndef HARKEN_CORE_SOURCE_CURSOR_MIRROR_H
#define HARKEN_CORE_SOURCE_CURSOR_MIRROR_H

#include "harken/core/SourcePhase.h"
#include "harken/core/StableRowStorage.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>
#include <type_traits>

namespace harken {

class SourceCursorMirror {
public:
    struct ConsumedProgress {
        std::uint64_t sourceReadFrame = 0;
        std::uint64_t sourceStartFrame = 0;
        ExactSourcePhase sourcePhase{};
        double currentRatio = 1.0;
        double targetRatio = 1.0;
        double rampStep = 0.0;
        std::uint32_t rampRemaining = 0;
        std::uint32_t activationLoopsRemaining = 0;
    };

    struct Row {
        std::uint64_t sourceFrame = 0; // Frames fed, distinct from consumption.
        std::uint64_t publicationSerial = 0;
        std::uint32_t renderGeneration = 0;
        std::uint32_t loopPassesRemaining = 0;
        bool active = false;
        bool exactTimingValid = false;
        bool awaitingStart = false;
        bool loopInfinite = false;
        bool hasExitMarker = false;
        ConsumedProgress progress{};
    };

    struct ParkingRow {
        std::uint64_t sourceFrame = 0;
        std::uint32_t renderGeneration = 0;
        std::uint32_t loopPassesRemaining = 0;
        bool active = false;
    };

    static_assert(sizeof(ConsumedProgress) == 64 && sizeof(Row) == 96);
    static_assert(sizeof(ParkingRow) == 24);
    static_assert(std::is_trivially_copyable_v<Row>);

    struct Publication {
        std::uint64_t blockIndex = 0;
        std::uint64_t outputFrame = 0;
        std::uint64_t consumedCommands = 0;
        std::uint64_t serial = 0;
        std::uint32_t availableCount = 0;
    };

    struct WriteView {
        Row* rows = nullptr;
        std::uint32_t count = 0;
        std::uint64_t serial = 0;
    };

    explicit SourceCursorMirror(std::uint32_t slotCount)
        : m_slotCount(slotCount == 0 ? 1 : slotCount) {
        for (auto& slot : m_slots)
            if (!slot.rows.Reserve(m_slotCount)) throw std::bad_alloc();
    }

    std::uint32_t SlotCount() const noexcept { return m_slotCount; }

    // Dependencies are prepared before VoicePool releases its constructed count.
    // Partial growth is retained for retry, but grants no new voice identity.
    bool Prepare(std::uint32_t count) noexcept {
        for (auto& slot : m_slots)
            if (!slot.rows.ConstructThrough(count)) return false;
        return true;
    }

    std::size_t ReservedBytes() const noexcept {
        return m_slots[0].rows.ReservedBytes() + m_slots[1].rows.ReservedBytes() +
            m_slots[2].rows.ReservedBytes();
    }

    std::size_t CommittedBytes() const noexcept {
        return m_slots[0].rows.CommittedBytes() + m_slots[1].rows.CommittedBytes() +
            m_slots[2].rows.CommittedBytes();
    }

    std::size_t RequestedBytes() const noexcept {
        return m_slots[0].rows.RequestedBytes() + m_slots[1].rows.RequestedBytes() +
            m_slots[2].rows.RequestedBytes();
    }

    void SetCommitPageLimitForTests(std::uint32_t slot, std::size_t pages) noexcept {
        assert(slot < kSlotCount);
        m_slots[slot].rows.SetCommitPageLimitForTests(pages);
    }

    // count is one acquired VoicePool prefix, held fixed through EndWrite.
    WriteView BeginWrite(std::uint32_t count) noexcept {
        const std::uint32_t pin = m_pin.load(std::memory_order_seq_cst);
        m_writeIndex = (m_lastPublished + 1u) % kSlotCount;
        if (m_writeIndex == pin) m_writeIndex = (m_writeIndex + 1u) % kSlotCount;
        assert(count <= m_slots[m_writeIndex].rows.ConstructedCount());
        m_writeCount = count;
        if (++m_writeSerial == 0) ++m_writeSerial;
        return {m_slots[m_writeIndex].rows.data(), count, m_writeSerial};
    }

    void EndWrite(std::uint64_t blockIndex, std::uint64_t outputFrame = 0,
                  std::uint64_t consumedCommands = 0) noexcept {
        Slot& slot = m_slots[m_writeIndex];
        slot.publication = {blockIndex, outputFrame, consumedCommands,
                            m_writeSerial, m_writeCount};
        m_published.store(m_writeIndex, std::memory_order_seq_cst);
        m_lastPublished = m_writeIndex;
    }

    bool CaptureRow(std::uint32_t index, Row* out, Publication* publication) const {
        if (out == nullptr || publication == nullptr) return false;
        return CapturePinned([&](const Slot& slot, const Publication& captured) {
            if (index >= captured.availableCount) return false;
            const Row& row = slot.rows.data()[index];
            *out = row.publicationSerial == captured.serial ? row : Row{};
            return true;
        }, publication);
    }

    bool CaptureParking(ParkingRow* out, std::uint32_t capacity,
                        Publication* publication) const {
        if (publication == nullptr) return false;
        return CapturePinned([&](const Slot& slot, const Publication& captured) {
            if (capacity < captured.availableCount ||
                (out == nullptr && captured.availableCount != 0)) return false;
            const Row* rows = slot.rows.data();
            for (std::uint32_t i = 0; i < captured.availableCount; ++i) {
                const Row& row = rows[i];
                out[i] = row.publicationSerial == captured.serial
                    ? ParkingRow{row.sourceFrame, row.renderGeneration,
                                 row.loopPassesRemaining, row.active}
                    : ParkingRow{};
            }
            return true;
        }, publication);
    }

private:
    template<class Capture>
    bool CapturePinned(Capture&& capture, Publication* publication) const {
        for (int attempt = 0; attempt < kCaptureAttempts; ++attempt) {
            const std::uint32_t candidate = m_published.load(std::memory_order_seq_cst);
            m_pin.store(candidate, std::memory_order_seq_cst);
            if (m_published.load(std::memory_order_seq_cst) != candidate) {
                m_pin.store(kNoPin, std::memory_order_release);
                continue;
            }
            const Slot& slot = m_slots[candidate];
            const Publication captured = slot.publication;
            const bool ok = capture(slot, captured);
            m_pin.store(kNoPin, std::memory_order_release);
            if (ok) *publication = captured;
            return ok;
        }
        m_pin.store(kNoPin, std::memory_order_release);
        return false;
    }

    static constexpr int kCaptureAttempts = 4;
    static constexpr std::uint32_t kSlotCount = 3;
    static constexpr std::uint32_t kNoPin = kSlotCount;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    struct Slot {
        StableRowStorage<Row> rows;
        Publication publication{};
    };

    std::uint32_t m_slotCount;
    Slot m_slots[kSlotCount];
    std::atomic<std::uint32_t> m_published{0};
    mutable std::atomic<std::uint32_t> m_pin{kNoPin};
    std::uint32_t m_lastPublished = 0;
    std::uint32_t m_writeIndex = 1;
    std::uint32_t m_writeCount = 0;
    std::uint64_t m_writeSerial = 0;
};

} // namespace harken

#endif // HARKEN_CORE_SOURCE_CURSOR_MIRROR_H
