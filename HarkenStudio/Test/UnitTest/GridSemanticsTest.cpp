#include "GridSemantics.h"
#include "MixingSemantics.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "VariableSemantics.h"

#include <gtest/gtest.h>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

using namespace VNX;
namespace GS = VNX::GridSemantics;

namespace {

QUuid uuid(int n)
{
    return QUuid(0xABC00000u + n, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

DiscreteGridDef makeDiscrete(int axisCount)
{
    DiscreteGridDef grid;
    for (int a = 0; a < axisCount; ++a)
        EXPECT_TRUE(grid.addAxis(uuid(100 + a)));
    return grid;
}


}

TEST(GridSemantics, ResolvedAxisValuesRequiresEveryAxisAndPreservesZero)
{
    const auto empty = GS::resolvedAxisValues({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    const auto resolved = GS::resolvedAxisValues({-0.5f, 0.0f, 1.25f});
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, (std::vector<float>{-0.5f, 0.0f, 1.25f}));
    EXPECT_FALSE(GS::resolvedAxisValues({std::nullopt, 0.0f}).has_value());
    EXPECT_FALSE(GS::resolvedAxisValues({0.0f, std::nullopt}).has_value());
    EXPECT_FALSE(GS::resolvedAxisValues({std::nullopt}).has_value());
}


TEST(GridSemantics, DiscreteSingleAxisPointBeatsWildcardAndMissesAreSilent)
{
    DiscreteGridDef grid = makeDiscrete(1);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.addRow(uuid(2)));
    ASSERT_TRUE(grid.setCell(0, 0, std::nullopt));
    ASSERT_TRUE(grid.setCell(1, 0, 3));

    const auto atThree = GS::evalDiscreteGrid(grid, {3.0f});
    EXPECT_EQ(atThree.winner, 1);
    EXPECT_EQ(atThree.hits, (std::vector<int>{0, 1}));

    const auto atFive = GS::evalDiscreteGrid(grid, {5.0f});
    EXPECT_EQ(atFive.winner, 0);
    EXPECT_EQ(atFive.hits, (std::vector<int>{0}));

    DiscreteGridDef noWildcard = makeDiscrete(1);
    ASSERT_TRUE(noWildcard.addRow(uuid(1)));
    ASSERT_TRUE(noWildcard.setCell(0, 0, 3));
    const auto miss = GS::evalDiscreteGrid(noWildcard, {5.0f});
    EXPECT_EQ(miss.winner, -1);
    EXPECT_TRUE(miss.hits.empty());
}

TEST(GridSemantics, DiscreteLexicographicTieBreakLeftmostAxisIsPrimaryKey)
{
    DiscreteGridDef grid = makeDiscrete(3);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.addRow(uuid(2)));
    ASSERT_TRUE(grid.setCell(0, 1, 5));
    ASSERT_TRUE(grid.setCell(0, 2, 7));
    ASSERT_TRUE(grid.setCell(1, 0, 3));

    // (wildcard, 5, 7) vs (3, wildcard, wildcard): the leftmost axis decides,
    // secondary axes must not gang up on the primary key.
    const auto result = GS::evalDiscreteGrid(grid, {3.0f, 5.0f, 7.0f});
    EXPECT_EQ(result.hits, (std::vector<int>{0, 1}));
    EXPECT_EQ(result.winner, 1);
}

TEST(GridSemantics, DiscreteDuplicatePatternsTakeTheFirstRow)
{
    DiscreteGridDef grid = makeDiscrete(1);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.addRow(uuid(2)));
    ASSERT_TRUE(grid.setCell(0, 0, 2));
    ASSERT_TRUE(grid.setCell(1, 0, 2));

    const auto result = GS::evalDiscreteGrid(grid, {2.0f});
    EXPECT_EQ(result.winner, 0);
    EXPECT_EQ(result.hits, (std::vector<int>{0, 1}));
}

TEST(GridSemantics, DiscreteReadRoundsAtAnchorWindowBoundaries)
{
    DiscreteGridDef grid = makeDiscrete(1);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.setCell(0, 0, 1));

    EXPECT_EQ(GS::evalDiscreteGrid(grid, {0.5f}).winner, 0);
    EXPECT_EQ(GS::evalDiscreteGrid(grid, {1.49f}).winner, 0);
    EXPECT_EQ(GS::evalDiscreteGrid(grid, {0.49f}).winner, -1);
    EXPECT_EQ(GS::evalDiscreteGrid(grid, {1.5f}).winner, -1);
}

TEST(GridSemantics, MoveAxisReordersCellLedgerAndTieBreakPriority)
{
    DiscreteGridDef grid = makeDiscrete(2);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.addRow(uuid(2)));
    ASSERT_TRUE(grid.setCell(0, 0, 3));
    ASSERT_TRUE(grid.setCell(1, 1, 5));

    EXPECT_EQ(GS::evalDiscreteGrid(grid, {3.0f, 5.0f}).winner, 0);

    ASSERT_TRUE(grid.moveAxis(0, 1));
    EXPECT_EQ(grid.axes[0], uuid(101));
    EXPECT_EQ(grid.axes[1], uuid(100));
    EXPECT_EQ(grid.cellAt(0, 1), std::optional<int>(3));
    EXPECT_FALSE(grid.cellAt(0, 0).has_value());
    EXPECT_EQ(grid.cellAt(1, 0), std::optional<int>(5));

    EXPECT_EQ(GS::evalDiscreteGrid(grid, {5.0f, 3.0f}).winner, 1);
}

TEST(GridSemantics, LedgerMutationsKeepCellsInSyncAndRejectInvalidWrites)
{
    DiscreteGridDef grid = makeDiscrete(1);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.setCell(0, 0, 4));

    EXPECT_FALSE(grid.addAxis(QUuid()));
    EXPECT_FALSE(grid.addAxis(uuid(100)));
    EXPECT_FALSE(grid.addRow(QUuid()));
    EXPECT_FALSE(grid.setCell(0, 1, 2));
    EXPECT_FALSE(grid.setCell(1, 0, 2));
    EXPECT_FALSE(grid.removeAxis(1));
    EXPECT_FALSE(grid.removeRow(1));
    EXPECT_FALSE(grid.moveAxis(0, 1));

    ASSERT_TRUE(grid.addAxis(uuid(101)));
    ASSERT_EQ(grid.rows[0].cells.size(), 2u);
    EXPECT_FALSE(grid.rows[0].cells[1].has_value());
    EXPECT_EQ(grid.cellAt(0, 0), std::optional<int>(4));

    ASSERT_TRUE(grid.removeAxis(0));
    ASSERT_EQ(grid.axes.size(), 1u);
    EXPECT_EQ(grid.axes[0], uuid(101));
    ASSERT_EQ(grid.rows[0].cells.size(), 1u);
    EXPECT_FALSE(grid.rows[0].cells[0].has_value());

    ASSERT_TRUE(grid.addRow(uuid(1)));
    EXPECT_EQ(grid.rows[1].cells.size(), 1u);
    ASSERT_TRUE(grid.removeRow(0));
    EXPECT_EQ(grid.rows.size(), 1u);
}

TEST(GridSemantics, LintSplitsIdenticalCellsByTargetIntoDupAndDead)
{
    DiscreteGridDef grid = makeDiscrete(2);
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.addRow(uuid(1)));
    ASSERT_TRUE(grid.addRow(uuid(2)));
    ASSERT_TRUE(grid.addRow(uuid(3)));
    for (int r = 0; r < 3; ++r) {
        ASSERT_TRUE(grid.setCell(r, 0, 1));
        ASSERT_TRUE(grid.setCell(r, 1, std::nullopt));
    }
    ASSERT_TRUE(grid.setCell(3, 0, 1));
    ASSERT_TRUE(grid.setCell(3, 1, 2));

    const auto findings = GS::lintDiscreteGrid(grid);
    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].row, 1);
    EXPECT_EQ(findings[0].blockingRow, 0);
    EXPECT_EQ(findings[0].kind, GS::GridLintKind::DuplicateRow);
    EXPECT_EQ(findings[1].row, 2);
    EXPECT_EQ(findings[1].blockingRow, 0);
    EXPECT_EQ(findings[1].kind, GS::GridLintKind::UnreachableRow);
}

TEST(GridSemantics, MountCriterionReadsDiscreteTableAndTimingOnly)
{
    DiscreteGridDef grid;
    EXPECT_EQ(GS::gridMountState(grid, VariableResponse::AtStart), GS::GridMountState::None);

    ASSERT_TRUE(grid.addAxis(uuid(100)));
    EXPECT_EQ(GS::gridMountState(grid, VariableResponse::AtStart), GS::GridMountState::Idle);

    EXPECT_EQ(GS::gridMountState(grid, VariableResponse::Live), GS::GridMountState::Editable);
}

TEST(GridSemantics, KindNeverGatesConsumptionAcrossTheWholeFamily)
{
    VVariable wind;
    wind.kind = VariableKind::Smooth;
    wind.rangeMin = 0.0f;
    wind.rangeMax = 10.0f;

    DiscreteGridDef discrete = makeDiscrete(1);
    ASSERT_TRUE(discrete.addRow(uuid(1)));
    ASSERT_TRUE(discrete.setCell(0, 0, 4));

    MixingChildDef continuous{uuid(1), {{0.0f, 1.0f}, {5.0f, 1.0f}, {6.0f, 0.0f}}, {}};
    continuous.normalize();

    const float smoothValue = 3.6f;
    EXPECT_EQ(GS::evalDiscreteGrid(discrete, {smoothValue}).winner, 0);
    EXPECT_FLOAT_EQ(continuous.gainAt(smoothValue), 1.0f);

    ASSERT_TRUE(wind.setField(QStringLiteral("kind"),
                              static_cast<int>(VariableKind::Step)));
    const float stepped = static_cast<float>(
        VariableSemantics::discreteRead(smoothValue));
    EXPECT_EQ(GS::evalDiscreteGrid(discrete, {stepped}).winner, 0);
    EXPECT_FLOAT_EQ(continuous.gainAt(stepped), 1.0f);
    EXPECT_FLOAT_EQ(continuous.gainAt(6.0f), 0.0f);
}

TEST(GridSemantics, RemapGridTargetsRewritesBothTablesWithoutTouchingCells)
{
    DiscreteGridDef discrete = makeDiscrete(1);
    ASSERT_TRUE(discrete.addRow(uuid(1)));
    ASSERT_TRUE(discrete.addRow(uuid(2)));
    ASSERT_TRUE(discrete.setCell(0, 0, 4));
    ASSERT_TRUE(discrete.setCell(1, 0, 8));

    MixingDef continuous{{{uuid(200), {MixingChildDef::unity(uuid(1), 0.0f, 1.0f)}}}};

    QHash<QUuid, QUuid> guidMap;
    guidMap.insert(uuid(1), uuid(11));
    guidMap.insert(uuid(3), uuid(13));

    EXPECT_TRUE(GS::remapGridTargets(discrete, guidMap));
    MixingSemantics::remapTargets(continuous, guidMap);
    EXPECT_EQ(discrete.rows[0].target, uuid(11));
    EXPECT_EQ(discrete.rows[1].target, uuid(2));
    EXPECT_EQ(discrete.cellAt(0, 0), std::optional<int>(4));
    EXPECT_EQ(discrete.cellAt(1, 0), std::optional<int>(8));
    EXPECT_EQ(continuous.rows[0].children[0].target, uuid(11));
    EXPECT_FLOAT_EQ(continuous.rows[0].children[0].points.front().x, 0.0f);
    EXPECT_FLOAT_EQ(continuous.rows[0].children[0].points.back().x, 1.0f);
}
