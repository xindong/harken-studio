#pragma once
#include "harken/core/CommandQueue.h"

namespace harken::detail {
struct CommandQueueSubmissionTestAccess {
    static void OverwriteAt(CommandQueue& queue, std::uint32_t offset, const CommandRecord& record) {
        assert(offset < queue.PublishedHead() - queue.ConsumedCount());
        queue.m_slots[(queue.m_tail.load(std::memory_order_relaxed) + offset) & queue.m_mask] = record;
    }
    static void OverwriteFront(CommandQueue& queue, const CommandRecord& record) {
        queue.m_slots[queue.m_tail.load(std::memory_order_relaxed) & queue.m_mask] = record;
    }
    static void ExhaustFrozenOrigins(CommandQueue& queue) {
        queue.m_nextFrozenOrigin = UINT64_MAX;
    }
};
}
