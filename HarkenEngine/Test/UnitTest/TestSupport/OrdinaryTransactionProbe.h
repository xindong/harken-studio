#pragma once

#include "harken/core/ScheduledTransactionPool.h"

namespace harken::detail {

struct OrdinaryTransactionProbeForTest {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t matchingRecords = 0;
    std::uint32_t remainingEvents = 0;
    std::uint32_t matchingPageEvents = 0;
    std::uint32_t pageEventCount = 0;
    std::uint32_t pageFrameBatchCount = 0;
    std::uint32_t committedTransactionCount = 0;
    std::uint32_t liveTransactionCount = 0;
    std::uint64_t pageGeneration = 0;
    bool pageFrozen = false;
};

bool CaptureOrdinaryTransactionForTest(
    const ScheduledTransactionPool& pool,
    TimelineTransactionProgramRef program,
    std::uint64_t absoluteQuantum,
    OrdinaryTransactionProbeForTest* out) noexcept;

} // namespace harken::detail
