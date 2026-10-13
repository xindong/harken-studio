#include "harken/core/BlockArena.h"

#include <cstring>

namespace harken {

BlockArena::BlockArena(std::uint32_t capacityBytes)
    : m_buffer(std::make_unique<std::byte[]>(capacityBytes)),
      m_storageCapacity(capacityBytes),
      m_capacity(capacityBytes) {}

void BlockArena::NoteHighWater() {
    const std::uint32_t inUse = m_lowUsed + m_highUsed;
    if (inUse > m_highWater) {
        m_highWater = inUse;
    }
}

void* BlockArena::Alloc(std::size_t bytes, std::size_t alignment,
                        bool fromHighEnd) {
    const std::size_t base = reinterpret_cast<std::size_t>(m_buffer.get());
    if (bytes == 0) {
        if (!fromHighEnd) {
            const std::size_t current = base + m_lowUsed;
            const std::size_t aligned =
                (current + alignment - 1) & ~(alignment - 1);
            return reinterpret_cast<void*>(aligned);
        }
        const std::size_t limit = base + m_capacity - m_highUsed;
        const std::size_t aligned = limit & ~(alignment - 1);
        return reinterpret_cast<void*>(aligned);
    }
    if (!fromHighEnd) {
        const std::size_t current = base + m_lowUsed;
        const std::size_t aligned =
            (current + alignment - 1) & ~(alignment - 1);
        const std::size_t newUsed = (aligned - base) + bytes;
        if (newUsed + m_highUsed > m_capacity) {
            ++m_overflows;
            return nullptr;
        }
        m_lowUsed = static_cast<std::uint32_t>(newUsed);
        NoteHighWater();
        return reinterpret_cast<void*>(aligned);
    }
    // The high end grows downwards, so its base rounds down. The size test has
    // to come first: `capacity - highUsed - bytes` on an oversized request
    // wraps into a huge value that looks like a perfectly good address.
    const std::size_t free = m_capacity - m_lowUsed - m_highUsed;
    if (bytes > free) {
        ++m_overflows;
        return nullptr;
    }
    const std::size_t limit = base + m_capacity - m_highUsed;
    const std::size_t aligned = (limit - bytes) & ~(alignment - 1);
    if (aligned < base + m_lowUsed) {
        ++m_overflows;
        return nullptr;
    }
    m_highUsed = static_cast<std::uint32_t>(base + m_capacity - aligned);
    NoteHighWater();
    return reinterpret_cast<void*>(aligned);
}

float* BlockArena::AllocFloats(std::size_t count) {
    void* p = Alloc(count * sizeof(float), alignof(float), false);
    if (p != nullptr) {
        std::memset(p, 0, count * sizeof(float));
    }
    return static_cast<float*>(p);
}

float* BlockArena::AllocFloatsHigh(std::size_t count) {
    void* p = Alloc(count * sizeof(float), alignof(float), true);
    if (p != nullptr) {
        std::memset(p, 0, count * sizeof(float));
    }
    return static_cast<float*>(p);
}

void BlockArena::Reset() {
    m_lowUsed = 0;
    m_highUsed = m_highFloor;
}

void BlockArena::QuarantineHighEnd() {
    // The span handed out since the last Reset is exactly [floor, highUsed],
    // and highUsed only grows within a block, so this is monotone: a second
    // call can never give back retired bytes.
    m_highFloor = m_highUsed;
}

void BlockArena::SetCapacityForTests(std::uint32_t capacityBytes) {
    m_capacity =
        capacityBytes < m_storageCapacity ? capacityBytes
                                          : m_storageCapacity;
}

void BlockArena::RestoreCapacityForTests() {
    m_capacity = m_storageCapacity;
}

} // namespace harken
