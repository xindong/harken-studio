#include "harken/core/VariableControlGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace harken {
namespace {

bool ReadDiscreteValue(float value, std::int32_t* out) noexcept {
    if (out == nullptr || !std::isfinite(value)) return false;
    const double rounded = std::floor(static_cast<double>(value) + 0.5);
    if (rounded < static_cast<double>((std::numeric_limits<std::int32_t>::min)()) ||
        rounded > static_cast<double>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    *out = static_cast<std::int32_t>(rounded);
    return true;
}

const CurveV1* FindCurve(const ContainerProgramViewV3& program,
                         std::uint32_t curveLocalId) noexcept {
    if (curveLocalId == UINT32_MAX || program.curveCount == 0 ||
        program.curves == nullptr) {
        return nullptr;
    }
    const auto* found = std::lower_bound(
        program.curves, program.curves + program.curveCount, curveLocalId,
        [](const CurveV1& curve, std::uint32_t id) {
            return curve.curveLocalId < id;
        });
    return found != program.curves + program.curveCount &&
                   found->curveLocalId == curveLocalId
               ? found
               : nullptr;
}

GridEvaluationStatus EvaluateContinuousRow(
    const ContainerProgramViewV3& program,
    const ContinuousGridRowViewV1& row,
    const float* values,
    float* outWeight) noexcept {
    float weight = 1.0f;
    for (std::uint32_t axis = 0; axis < row.cellCount; ++axis) {
        const float value = values[axis];
        if (!std::isfinite(value)) return GridEvaluationStatus::Invalid;
        const auto& cell = row.cells[axis];
        if (cell.kind == ContinuousCellKindV1::Unconstrained) continue;
        if (cell.kind != ContinuousCellKindV1::Interval) {
            return GridEvaluationStatus::Invalid;
        }
        const bool below = value < cell.lower ||
            (value == cell.lower && cell.lowerInclusive == 0);
        const bool above = value > cell.upper ||
            (value == cell.upper && cell.upperInclusive == 0);
        if (below || above) {
            weight = 0.0f;
            break;
        }
        if (cell.leftCurveLocalId != UINT32_MAX) {
            const CurveV1* curve = FindCurve(program, cell.leftCurveLocalId);
            if (curve == nullptr) return GridEvaluationStatus::Invalid;
            weight *= curve->curve.Eval(value);
        }
        if (cell.rightCurveLocalId != UINT32_MAX) {
            const CurveV1* curve = FindCurve(program, cell.rightCurveLocalId);
            if (curve == nullptr) return GridEvaluationStatus::Invalid;
            weight *= curve->curve.Eval(value);
        }
        if (!std::isfinite(weight) || weight < 0.0f) {
            return GridEvaluationStatus::Invalid;
        }
    }
    *outWeight = weight;
    return GridEvaluationStatus::Ok;
}

} // namespace

GridEvaluationStatus EvaluateDiscreteGrid(
    const ContainerProgramViewV3& program,
    const float* axisValues,
    std::uint32_t axisValueCount,
    DiscreteGridSelection* out) noexcept {
    if (out == nullptr || axisValueCount != program.discreteAxisCount ||
        (axisValueCount != 0 && axisValues == nullptr) ||
        (program.discreteAxisCount != 0 && program.discreteAxes == nullptr) ||
        (program.discreteRowCount != 0 && program.discreteRows == nullptr)) {
        return GridEvaluationStatus::Invalid;
    }
    for (std::uint32_t axis = 0; axis < program.discreteAxisCount; ++axis) {
        const std::uint32_t authoredOrder =
            program.discreteAxes[axis].authoredColumnOrder;
        if (authoredOrder >= program.discreteAxisCount) {
            return GridEvaluationStatus::Invalid;
        }
        for (std::uint32_t prior = 0; prior < axis; ++prior) {
            if (program.discreteAxes[prior].authoredColumnOrder ==
                authoredOrder) {
                return GridEvaluationStatus::Invalid;
            }
        }
    }
    const DiscreteGridRowViewV1* winner = nullptr;
    for (std::uint32_t rowIndex = 0; rowIndex < program.discreteRowCount;
         ++rowIndex) {
        const auto& row = program.discreteRows[rowIndex];
        if (row.cellCount != axisValueCount ||
            (row.cellCount != 0 && row.cells == nullptr)) {
            return GridEvaluationStatus::Invalid;
        }
        bool matches = true;
        for (std::uint32_t axis = 0; axis < axisValueCount; ++axis) {
            std::int32_t value = 0;
            if (!ReadDiscreteValue(axisValues[axis], &value)) {
                return GridEvaluationStatus::Invalid;
            }
            const auto& cell = row.cells[axis];
            if (cell.kind != DiscreteCellKindV1::Wildcard &&
                (cell.kind != DiscreteCellKindV1::Exact ||
                 cell.exactValue != value)) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (winner == nullptr) {
            winner = &row;
            continue;
        }
        for (std::uint32_t authoredOrder = 0;
             authoredOrder < axisValueCount; ++authoredOrder) {
            std::uint32_t axis = 0;
            while (program.discreteAxes[axis].authoredColumnOrder !=
                   authoredOrder) {
                ++axis;
            }
            const bool candidateExact =
                row.cells[axis].kind == DiscreteCellKindV1::Exact;
            const bool winnerExact =
                winner->cells[axis].kind == DiscreteCellKindV1::Exact;
            if (candidateExact == winnerExact) continue;
            if (candidateExact) winner = &row;
            break;
        }
    }
    DiscreteGridSelection result{};
    if (winner != nullptr) {
        result.matched = true;
        result.stableRowOrdinal = winner->stableRowOrdinal;
        result.childStableOrdinal = winner->childStableOrdinal;
    }
    *out = result;
    return GridEvaluationStatus::Ok;
}

GridEvaluationStatus EvaluateContinuousGrid(
    const ContainerProgramViewV3& program,
    const float* axisValues,
    std::uint32_t axisValueCount,
    ContinuousGridWeight* output,
    std::uint32_t outputCapacity,
    std::uint32_t* outCount) noexcept {
    if (outCount == nullptr || axisValueCount != program.continuousAxisCount ||
        (axisValueCount != 0 && axisValues == nullptr) ||
        (program.continuousRowCount != 0 &&
         program.continuousRows == nullptr) ||
        program.continuousNormalization > 1) {
        return GridEvaluationStatus::Invalid;
    }
    double sum = 0.0;
    std::uint32_t memberCount = 0;
    for (std::uint32_t rowIndex = 0; rowIndex < program.continuousRowCount;
         ++rowIndex) {
        const auto& row = program.continuousRows[rowIndex];
        bool firstOccurrence = true;
        for (std::uint32_t prior = 0; prior < rowIndex; ++prior)
            firstOccurrence = firstOccurrence &&
                program.continuousRows[prior].childStableOrdinal != row.childStableOrdinal;
        memberCount += firstOccurrence;
        if (row.cellCount != axisValueCount ||
            (row.cellCount != 0 && row.cells == nullptr)) {
            return GridEvaluationStatus::Invalid;
        }
        float weight = 0.0f;
        const auto status =
            EvaluateContinuousRow(program, row, axisValues, &weight);
        if (status != GridEvaluationStatus::Ok) return status;
        if (weight > 0.0f) {
            sum += weight;
            if (!std::isfinite(sum)) return GridEvaluationStatus::Invalid;
        }
    }
    if (memberCount > outputCapacity ||
        (memberCount != 0 && output == nullptr)) {
        return GridEvaluationStatus::Capacity;
    }
    std::uint32_t write = 0;
    for (std::uint32_t rowIndex = 0; rowIndex < program.continuousRowCount;
         ++rowIndex) {
        const auto& row = program.continuousRows[rowIndex];
        float weight = 0.0f;
        const auto status =
            EvaluateContinuousRow(program, row, axisValues, &weight);
        if (status != GridEvaluationStatus::Ok) return status;
        std::uint32_t member = 0;
        while (member < write && output[member].childStableOrdinal != row.childStableOrdinal) ++member;
        if (member == write) {
            output[write++] = {row.stableRowOrdinal, row.childStableOrdinal, weight};
        } else {
            output[member].weight += weight;
            if (!std::isfinite(output[member].weight)) return GridEvaluationStatus::Invalid;
        }
    }
    if (program.continuousNormalization == 1 && sum > 0.0) {
        for (std::uint32_t member = 0; member < write; ++member)
            output[member].weight = static_cast<float>(static_cast<double>(output[member].weight) / sum);
    }
    *outCount = write;
    return GridEvaluationStatus::Ok;
}

} // namespace harken
