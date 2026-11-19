#ifndef HARKEN_CORE_PLAN03_GROUP_IDENTITY_H
#define HARKEN_CORE_PLAN03_GROUP_IDENTITY_H

#include "harken/core/DecisionToken.h"
#include "harken/core/TimeCoordinates.h"

#include <cstddef>

namespace harken {

enum class Plan03GroupBacking : std::uint8_t {
    Page = 0,
    ImmediateLane = 1,
};

struct Plan03GroupSliceIdentity {
    std::uint64_t absoluteQuantum = 0;
    std::uint64_t pageGeneration = 0;
    DecisionToken decision{};
    std::uint16_t sliceIndex = 0;
    std::size_t committedRow = SIZE_MAX;
    std::uint32_t rowClaimGeneration = 0;
    TransportEpoch transportEpoch = kInvalidTransportEpoch;
    Plan03GroupBacking backing = Plan03GroupBacking::Page;

    bool Valid() const noexcept {
        return pageGeneration != 0 && decision.Valid();
    }
    bool Exact() const noexcept {
        return Valid() && committedRow != SIZE_MAX && rowClaimGeneration != 0 &&
            transportEpoch != kInvalidTransportEpoch;
    }
    bool operator==(const Plan03GroupSliceIdentity& other) const noexcept {
        return backing == other.backing && absoluteQuantum == other.absoluteQuantum &&
            pageGeneration == other.pageGeneration &&
            decision.slot == other.decision.slot && decision.generation == other.decision.generation &&
            sliceIndex == other.sliceIndex && committedRow == other.committedRow &&
            rowClaimGeneration == other.rowClaimGeneration && transportEpoch == other.transportEpoch;
    }
};

} // namespace harken

#endif // HARKEN_CORE_PLAN03_GROUP_IDENTITY_H
