#ifndef HARKEN_CORE_WEIGHT_SEGMENT_H
#define HARKEN_CORE_WEIGHT_SEGMENT_H

#include "harken/core/BankVersionToken.h"
#include "harken/core/DecisionToken.h"

#include <cstdint>

namespace harken {

enum class WeightSegmentStatus : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    IdentityExhausted = 2,
};

struct WeightSegmentV1 {
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    float startWeight = 0.0f;
    float endWeight = 0.0f;
    std::uint32_t segmentGeneration = 0;
    DecisionToken decision{};
    BankVersionToken bank{};
};

} // namespace harken

#endif // HARKEN_CORE_WEIGHT_SEGMENT_H
