
#pragma once

#include "CurveSemantics.h"

#include <QHash>
#include <QJsonObject>
#include <QUuid>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace VNX {

struct ProjectItem;
class VVariable;

enum class VariableResponse : uint8_t { AtStart = 0, Live = 1 };

// Ledger discipline: axis and row mutations and the cell ledger stay in sync
// through this single mutate family — every write entry (GUI / IPC / undo)
// goes through here. Structural violations reject (return false), value
// violations clamp inside the cell normalize.
template <typename Cell>
struct GridTableDef
{
    struct Row
    {
        QUuid target;
        std::vector<std::optional<Cell>> cells;
        std::optional<uint32_t> stableRuleOrdinal;
    };

    std::vector<QUuid> axes;
    std::vector<Row> rows;

    bool empty() const { return axes.empty() && rows.empty(); }

    int axisIndex(const QUuid& id) const
    {
        for (size_t i = 0; i < axes.size(); ++i)
            if (axes[i] == id) return static_cast<int>(i);
        return -1;
    }

    std::optional<Cell> cellAt(int row, int axis) const
    {
        if (row < 0 || row >= static_cast<int>(rows.size())) return std::nullopt;
        const auto& cells = rows[row].cells;
        if (axis < 0 || axis >= static_cast<int>(cells.size())) return std::nullopt;
        return cells[axis];
    }

    bool addAxis(const QUuid& id)
    {
        if (id.isNull() || axisIndex(id) >= 0) return false;
        axes.push_back(id);
        for (Row& r : rows)
            r.cells.resize(axes.size());
        return true;
    }

    bool removeAxis(int index)
    {
        if (index < 0 || index >= static_cast<int>(axes.size())) return false;
        axes.erase(axes.begin() + index);
        for (Row& r : rows) {
            if (index < static_cast<int>(r.cells.size()))
                r.cells.erase(r.cells.begin() + index);
        }
        return true;
    }

    bool moveAxis(int from, int to)
    {
        const int count = static_cast<int>(axes.size());
        if (from < 0 || from >= count || to < 0 || to >= count) return false;
        if (from == to) return true;
        const QUuid axis = axes[from];
        axes.erase(axes.begin() + from);
        axes.insert(axes.begin() + to, axis);
        for (Row& r : rows) {
            r.cells.resize(axes.size());
            std::optional<Cell> cell = std::move(r.cells[from]);
            r.cells.erase(r.cells.begin() + from);
            r.cells.insert(r.cells.begin() + to, std::move(cell));
        }
        return true;
    }

    bool addRow(const QUuid& target)
    {
        if (target.isNull()) return false;
        Row r;
        r.target = target;
        r.cells.resize(axes.size());
        rows.push_back(std::move(r));
        return true;
    }

    bool removeRow(int index)
    {
        if (index < 0 || index >= static_cast<int>(rows.size())) return false;
        rows.erase(rows.begin() + index);
        return true;
    }

protected:
    bool assignCell(int row, int axis, std::optional<Cell> cell)
    {
        if (row < 0 || row >= static_cast<int>(rows.size())) return false;
        if (axis < 0 || axis >= static_cast<int>(axes.size())) return false;
        rows[row].cells.resize(axes.size());
        rows[row].cells[axis] = std::move(cell);
        return true;
    }
};

struct DiscreteGridCell
{
    int legacyValue = 0;
    QUuid labelId;

    DiscreteGridCell() = default;
    DiscreteGridCell(int value) : legacyValue(value) {}
    DiscreteGridCell(const QUuid& id) : labelId(id) {}
    bool operator==(const DiscreteGridCell& other) const
    {
        return labelId == other.labelId && (!labelId.isNull() || legacyValue == other.legacyValue);
    }
    bool operator!=(const DiscreteGridCell& other) const { return !(*this == other); }
};

struct DiscreteGridDef : GridTableDef<DiscreteGridCell>
{
    bool setCell(int row, int axis, std::optional<DiscreteGridCell> cell)
    {
        return assignCell(row, axis, std::move(cell));
    }

    QJsonObject toJson() const;
    static DiscreteGridDef fromJson(const QJsonObject& obj);
};

namespace GridSemantics {

using VariableLookup = std::function<const VVariable*(const QUuid&)>;
std::optional<int> discreteCellValue(const DiscreteGridCell& cell, const VVariable* variable);

bool isGridTargetCandidate(const ProjectItem& owner, const QUuid& target);
bool remapGridTargets(DiscreteGridDef& grid, const QHash<QUuid, QUuid>& guidMap);
bool validateOwnedGridTargets(const ProjectItem& owner, QString* error = nullptr);

std::optional<std::vector<float>> resolvedAxisValues(
    const std::vector<std::optional<float>>& values);

// Discrete read = round to the nearest integer (same window as label
// anchors); continuous read = raw value. Both reads live at the consumer, so
// Variable kind never gates an axis (blueprint 03).

struct DiscreteGridResult
{
    int winner = -1;
    std::vector<int> hits;
};

// Conjunctive hit (absent cell = wildcard), then axis-order lexicographic
// tie-break: first axis where exactly one contender holds a precise cell
// decides; identical patterns keep the earlier row (duplicate rows take
// first). No hit = silence, no implicit default.
DiscreteGridResult evalDiscreteGrid(const DiscreteGridDef& grid,
                                    const std::vector<float>& axisValues,
                                    const VariableLookup& variables = {});

enum class GridLintKind : uint8_t { DuplicateRow = 0, UnreachableRow = 1 };

struct GridLintFinding
{
    int row = -1;
    int blockingRow = -1;
    GridLintKind kind = GridLintKind::DuplicateRow;
};

// Under axis-order lexicographic tie-break, "covered and always beaten"
// collapses to "identical cells on every axis": same target = duplicate row
// (runtime takes the first, later ones inert), different target =
// unreachable row (the earlier row always wins).
std::vector<GridLintFinding> lintDiscreteGrid(const DiscreteGridDef& grid);

enum class GridMountState : uint8_t { None = 0, Idle = 1, Editable = 2 };

GridMountState gridMountState(const DiscreteGridDef& grid, VariableResponse response);

}
}
