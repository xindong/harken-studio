#ifndef HARKEN_CORE_VARIABLE_CONTROL_GRID_H
#define HARKEN_CORE_VARIABLE_CONTROL_GRID_H

#include "harken/core/CurrentWorld.h"

#include <cstdint>

namespace harken {

enum class GridEvaluationStatus : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    Capacity = 2,
};

struct DiscreteGridSelection {
    bool matched = false;
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t childStableOrdinal = 0;
};

struct ContinuousGridWeight {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t childStableOrdinal = 0;
    float weight = 0.0f;
};

GridEvaluationStatus EvaluateDiscreteGrid(
    const ContainerProgramViewV3& program,
    const float* axisValues,
    std::uint32_t axisValueCount,
    DiscreteGridSelection* out) noexcept;

GridEvaluationStatus EvaluateContinuousGrid(
    const ContainerProgramViewV3& program,
    const float* axisValues,
    std::uint32_t axisValueCount,
    ContinuousGridWeight* output,
    std::uint32_t outputCapacity,
    std::uint32_t* outCount) noexcept;

} // namespace harken

#endif // HARKEN_CORE_VARIABLE_CONTROL_GRID_H
