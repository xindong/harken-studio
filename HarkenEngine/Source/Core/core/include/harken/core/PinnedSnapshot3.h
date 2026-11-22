// Fixed-work audio-to-logic publication for one trivially-copyable payload.
// The reader pins and confirms a published slot before copying it. The writer
// never selects the published or pinned slot, so payload bytes are never read
// and written concurrently. Control words are sequentially consistent to keep
// the protocol valid on every supported target.
#ifndef HARKEN_CORE_PINNED_SNAPSHOT3_H
#define HARKEN_CORE_PINNED_SNAPSHOT3_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace harken {

template <typename Payload>
class PinnedSnapshot3 {
public:
    static_assert(std::is_trivially_copyable_v<Payload>,
                  "published payloads must cross threads by plain copy");

    Payload& BeginWrite() {
        const std::uint32_t pin = m_pin.load(std::memory_order_seq_cst);
        m_writeIndex = (m_lastPublished + 1u) % kSlotCount;
        if (m_writeIndex == pin) {
            m_writeIndex = (m_writeIndex + 1u) % kSlotCount;
        }
        return m_slots[m_writeIndex];
    }

    void EndWrite() {
        m_published.store(m_writeIndex, std::memory_order_seq_cst);
        m_lastPublished = m_writeIndex;
    }

    // Owner-only initialization before either thread can access this object.
    // Indirect storage must remain alive and retain one buffer per slot.
    void Initialize(const std::array<Payload, 3>& slots) noexcept {
        for (std::uint32_t i = 0; i < kSlotCount; ++i) m_slots[i] = slots[i];
    }

    // Copy indirect payload bytes while the slot is still pinned. The callback
    // must not retain references or pointers into the published storage.
    template<class Copy>
    bool CaptureWith(Copy&& copy) const noexcept {
        static_assert(std::is_nothrow_invocable_v<Copy, const Payload&>);
        for (int attempt = 0; attempt < kCaptureAttempts; ++attempt) {
            const std::uint32_t candidate = m_published.load(std::memory_order_seq_cst);
            m_pin.store(candidate, std::memory_order_seq_cst);
            if (m_published.load(std::memory_order_seq_cst) != candidate) {
                m_pin.store(kNoPin, std::memory_order_release);
                continue;
            }
            copy(static_cast<const Payload&>(m_slots[candidate]));
            m_pin.store(kNoPin, std::memory_order_release);
            return true;
        }
        m_pin.store(kNoPin, std::memory_order_release);
        return false;
    }

    bool Capture(Payload& out) const noexcept {
        return CaptureWith([&](const Payload& value) noexcept {
            std::memcpy(&out, &value, sizeof out);
        });
    }

private:
    static constexpr int kCaptureAttempts = 4;
    static constexpr std::uint32_t kSlotCount = 3;
    static constexpr std::uint32_t kNoPin = kSlotCount;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "snapshot control words must be lock-free");

    Payload m_slots[kSlotCount] = {};
    std::atomic<std::uint32_t> m_published{0};
    mutable std::atomic<std::uint32_t> m_pin{kNoPin};
    std::uint32_t m_lastPublished = 0;
    std::uint32_t m_writeIndex = 1;
};

} // namespace harken

#endif // HARKEN_CORE_PINNED_SNAPSHOT3_H
