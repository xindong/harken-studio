#include "Common/ObjectIdTestSupport.h"
#include "harken/core/VariableControlGrid.h"

#include <gtest/gtest.h>

#include <array>

namespace {

TEST(VariableControlGrid, DiscreteUsesExactWildcardLexicographicPriority) {
    const harken::GridAxisV1 axes[2] = {{0, 0, harkentest::ObjectId(11)}, {1, 1, harkentest::ObjectId(12)}};
    const harken::DiscreteGridCellV1 cells[3][2] = {
        {{harken::DiscreteCellKindV1::Wildcard, 0},
         {harken::DiscreteCellKindV1::Exact, 4}},
        {{harken::DiscreteCellKindV1::Exact, 3},
         {harken::DiscreteCellKindV1::Wildcard, 0}},
        {{harken::DiscreteCellKindV1::Exact, 3},
         {harken::DiscreteCellKindV1::Exact, 4}}};
    harken::DiscreteGridRowViewV1 rows[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        rows[i].stableRowOrdinal = i + 1;
        rows[i].authoredStableOrder = i;
        rows[i].childStableOrdinal = i + 10;
        rows[i].cellCount = 2;
        rows[i].cells = cells[i];
    }
    harken::ContainerProgramViewV3 program{};
    program.discreteAxisCount = 2;
    program.discreteAxes = axes;
    program.discreteRowCount = 3;
    program.discreteRows = rows;
    const float values[2] = {2.5f, 4.0f};
    harken::DiscreteGridSelection result{true, 99, 99};

    ASSERT_EQ(harken::GridEvaluationStatus::Ok,
              harken::EvaluateDiscreteGrid(program, values, 2, &result));
    ASSERT_TRUE(result.matched);
    EXPECT_EQ(3u, result.stableRowOrdinal);
    EXPECT_EQ(12u, result.childStableOrdinal);
}

TEST(VariableControlGrid,
     DiscreteLexicographicPriorityUsesAuthoredColumnOrder) {
    const harken::GridAxisV1 axes[2] = {{10, 1, harkentest::ObjectId(11)}, {20, 0, harkentest::ObjectId(12)}};
    const harken::DiscreteGridCellV1 cells[2][2] = {
        {{harken::DiscreteCellKindV1::Exact, 3},
         {harken::DiscreteCellKindV1::Wildcard, 0}},
        {{harken::DiscreteCellKindV1::Wildcard, 0},
         {harken::DiscreteCellKindV1::Exact, 4}}};
    harken::DiscreteGridRowViewV1 rows[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        rows[i].stableRowOrdinal = i + 1;
        rows[i].childStableOrdinal = i + 20;
        rows[i].cellCount = 2;
        rows[i].cells = cells[i];
    }
    harken::ContainerProgramViewV3 program{};
    program.discreteAxisCount = 2;
    program.discreteAxes = axes;
    program.discreteRowCount = 2;
    program.discreteRows = rows;
    const float values[2] = {3.0f, 4.0f};
    harken::DiscreteGridSelection result{};

    ASSERT_EQ(harken::GridEvaluationStatus::Ok,
              harken::EvaluateDiscreteGrid(program, values, 2, &result));
    ASSERT_TRUE(result.matched);
    EXPECT_EQ(2u, result.stableRowOrdinal);
    EXPECT_EQ(21u, result.childStableOrdinal);
}

TEST(VariableControlGrid, ContinuousProductsThenNormalizesPositiveRows) {
    const harken::GridAxisV1 axis{0, 0, harkentest::ObjectId(11)};
    const harken::ContinuousGridCellV1 cells[2] = {
        {harken::ContinuousCellKindV1::Interval,
         0.0f, 1.0f, 1, 1, 0, UINT32_MAX, UINT32_MAX},
        {harken::ContinuousCellKindV1::Interval,
         0.5f, 2.0f, 1, 1, 0, UINT32_MAX, UINT32_MAX}};
    harken::ContinuousGridRowViewV1 rows[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        rows[i].stableRowOrdinal = i;
        rows[i].childStableOrdinal = i + 20;
        rows[i].cellCount = 1;
        rows[i].cells = &cells[i];
    }
    harken::ContainerProgramViewV3 program{};
    program.continuousNormalization = 1;
    program.continuousAxisCount = 1;
    program.continuousAxes = &axis;
    program.continuousRowCount = 2;
    program.continuousRows = rows;
    const float value = 0.75f;
    std::array<harken::ContinuousGridWeight, 2> weights{};
    std::uint32_t weightCount = 0;

    ASSERT_EQ(harken::GridEvaluationStatus::Ok,
              harken::EvaluateContinuousGrid(program, &value, 1,
                                            weights.data(), weights.size(),
                                            &weightCount));
    ASSERT_EQ(2u, weightCount);
    EXPECT_FLOAT_EQ(0.5f, weights[0].weight);
    EXPECT_FLOAT_EQ(0.5f, weights[1].weight);
}

TEST(VariableControlGrid, ContinuousIncludesSilentRowsAndCountsTheirCapacity) {
    const harken::ContinuousGridCellV1 cells[2] = {
        {harken::ContinuousCellKindV1::Interval, 0.0f, 1.0f, 1, 1},
        {harken::ContinuousCellKindV1::Interval, 2.0f, 3.0f, 1, 1}};
    const harken::ContinuousGridRowViewV1 rows[2] = {
        {0, 0, 7, 1, &cells[0]}, {1, 1, 9, 1, &cells[1]}};
    harken::ContainerProgramViewV3 program{};
    program.continuousAxisCount = 1;
    program.continuousRowCount = 2;
    program.continuousRows = rows;
    program.continuousNormalization = 1;
    std::array<harken::ContinuousGridWeight, 2> weights{};
    std::uint32_t count = 0;
    const float value = 1.5f;
    EXPECT_EQ(harken::GridEvaluationStatus::Capacity,
        harken::EvaluateContinuousGrid(program, &value, 1, weights.data(), 1, &count));
    ASSERT_EQ(harken::GridEvaluationStatus::Ok,
        harken::EvaluateContinuousGrid(program, &value, 1, weights.data(), 2, &count));
    ASSERT_EQ(2u, count);
    EXPECT_FLOAT_EQ(0.0f, weights[0].weight);
    EXPECT_FLOAT_EQ(0.0f, weights[1].weight);
}

TEST(VariableControlGrid, ContinuousWithoutAxesIncludesEveryRowAtUnity) {
    const harken::ContinuousGridRowViewV1 rows[2] = {
        {0, 0, 7, 0, nullptr}, {1, 1, 9, 0, nullptr}};
    harken::ContainerProgramViewV3 program{};
    program.continuousRowCount = 2;
    program.continuousRows = rows;
    std::array<harken::ContinuousGridWeight, 2> weights{};
    std::uint32_t count = 0;
    ASSERT_EQ(harken::GridEvaluationStatus::Ok,
        harken::EvaluateContinuousGrid(program, nullptr, 0, weights.data(), 2, &count));
    ASSERT_EQ(2u, count);
    EXPECT_FLOAT_EQ(1.0f, weights[0].weight);
    EXPECT_FLOAT_EQ(1.0f, weights[1].weight);
}

TEST(VariableControlGrid, RepeatedChildRowsSumIntoOneMemberIncludingOverlaps) {
    const harken::ContinuousGridRowViewV1 rows[3] = {
        {0, 0, 7, 0, nullptr}, {1, 1, 9, 0, nullptr}, {2, 2, 7, 0, nullptr}};
    harken::ContainerProgramViewV3 program{};
    program.continuousRowCount = 3;
    program.continuousRows = rows;
    std::array<harken::ContinuousGridWeight, 2> weights{};
    std::uint32_t count = 0;
    ASSERT_EQ(harken::GridEvaluationStatus::Ok,
        harken::EvaluateContinuousGrid(program, nullptr, 0, weights.data(), 2, &count));
    ASSERT_EQ(2u, count);
    EXPECT_EQ(7u, weights[0].childStableOrdinal);
    EXPECT_FLOAT_EQ(2.0f, weights[0].weight);
    EXPECT_EQ(9u, weights[1].childStableOrdinal);
    EXPECT_FLOAT_EQ(1.0f, weights[1].weight);
}

} // namespace
