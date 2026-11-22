#ifndef HARKEN_CORE_TIMELINE_EXECUTION_ORDER_H
#define HARKEN_CORE_TIMELINE_EXECUTION_ORDER_H

#include <cstdint>
#include <tuple>

namespace harken {

struct TimelineExecutionOrder {
    std::uint64_t executeFrame = 0;
    std::uint64_t logicCommitSequence = 0;
    std::uint32_t executionOrdinal = 0;
    std::uint32_t eventIndex = 0;
    std::uint32_t transactionOrdinal = 0;
    std::uint32_t subeventPhase = 0;
    std::uint64_t rowSequence = 0;

    bool operator==(const TimelineExecutionOrder&) const = default;

    bool operator<(const TimelineExecutionOrder& other) const noexcept {
        return std::tie(executeFrame, logicCommitSequence, executionOrdinal,
                        eventIndex, transactionOrdinal, subeventPhase, rowSequence) <
            std::tie(other.executeFrame, other.logicCommitSequence,
                     other.executionOrdinal, other.eventIndex,
                     other.transactionOrdinal, other.subeventPhase, other.rowSequence);
    }
};

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_EXECUTION_ORDER_H
