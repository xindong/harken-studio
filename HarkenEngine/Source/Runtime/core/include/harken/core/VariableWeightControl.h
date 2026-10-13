#ifndef HARKEN_CORE_VARIABLE_WEIGHT_CONTROL_H
#define HARKEN_CORE_VARIABLE_WEIGHT_CONTROL_H

#include "harken/core/WeightSegment.h"

namespace harken {

struct VariableWeightControlIdentity {
    std::uint64_t instance = 0;
    std::uint64_t scheduleEpoch = 0;
    std::uint8_t side = UINT8_MAX;

    bool Valid() const noexcept {
        return instance != 0 && scheduleEpoch != 0 && side < 2;
    }
    bool operator==(const VariableWeightControlIdentity&) const = default;
};

struct VariableWeightControlBinding {
    VariableWeightControlIdentity owner{};
    WeightSegmentV1 segment{};
};

} // namespace harken

#endif
