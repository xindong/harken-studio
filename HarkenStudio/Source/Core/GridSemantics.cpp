
#include "GridSemantics.h"
#include "Components/VVariable.h"

#include "Components/VSelection.h"
#include "ProjectTypes.h"
#include "PlaybackReferenceValidation.h"
#include "VariableSemantics.h"

#include <QJsonArray>

#include <algorithm>

namespace VNX {

namespace {

QUuid guidFromJson(const QJsonValue& v)
{
    return QUuid::fromString(v.toString());
}

QJsonArray axesToJson(const std::vector<QUuid>& axes)
{
    QJsonArray arr;
    for (const QUuid& axis : axes)
        arr.append(axis.toString(QUuid::WithoutBraces));
    return arr;
}

std::vector<QUuid> axesFromJson(const QJsonValue& v)
{
    std::vector<QUuid> axes;
    for (const QJsonValue& e : v.toArray()) {
        const QUuid id = guidFromJson(e);
        if (!id.isNull())
            axes.push_back(id);
    }
    return axes;
}

}

QJsonObject DiscreteGridDef::toJson() const
{
    QJsonObject obj;
    obj["axes"] = axesToJson(axes);
    QJsonArray rowsArr;
    for (const Row& r : rows) {
        QJsonObject ro;
        ro["target"] = r.target.toString(QUuid::WithoutBraces);
        if (r.stableRuleOrdinal)
            ro["stableRuleOrdinal"] = static_cast<qint64>(*r.stableRuleOrdinal);
        QJsonArray cells;
        for (size_t a = 0; a < axes.size(); ++a) {
            const std::optional<DiscreteGridCell> cell =
                a < r.cells.size() ? r.cells[a] : std::nullopt;
            if (!cell)
                cells.append(QJsonValue());
            else if (cell->labelId.isNull())
                cells.append(cell->legacyValue);
            else
                cells.append(QJsonObject{{QStringLiteral("labelId"),
                    cell->labelId.toString(QUuid::WithoutBraces)}});
        }
        ro["cells"] = cells;
        rowsArr.append(ro);
    }
    obj["rows"] = rowsArr;
    return obj;
}

DiscreteGridDef DiscreteGridDef::fromJson(const QJsonObject& obj)
{
    DiscreteGridDef def;
    def.axes = axesFromJson(obj["axes"]);
    for (const QJsonValue& v : obj["rows"].toArray()) {
        const QJsonObject ro = v.toObject();
        const QUuid target = guidFromJson(ro["target"]);
        Row r;
        r.target = target;
        if (ro.contains(QStringLiteral("stableRuleOrdinal")))
            r.stableRuleOrdinal = static_cast<uint32_t>(
                ro.value(QStringLiteral("stableRuleOrdinal")).toInteger());
        for (const QJsonValue& c : ro["cells"].toArray())
            r.cells.push_back(c.isNull() ? std::nullopt
                : std::optional<DiscreteGridCell>(c.isObject()
                    ? DiscreteGridCell(QUuid(c.toObject().value(QStringLiteral("labelId")).toString()))
                    : DiscreteGridCell(c.toInt())));
        r.cells.resize(def.axes.size());
        def.rows.push_back(std::move(r));
    }
    return def;
}

namespace GridSemantics {

std::optional<int> discreteCellValue(const DiscreteGridCell& cell, const VVariable* variable)
{
    if (cell.labelId.isNull()) return cell.legacyValue;
    const auto* label = variable ? VariableSemantics::labelById(variable->labels, cell.labelId) : nullptr;
    return label ? std::optional<int>(label->anchor) : std::nullopt;
}

std::optional<std::vector<float>> resolvedAxisValues(
    const std::vector<std::optional<float>>& values)
{
    std::vector<float> resolved;
    resolved.reserve(values.size());
    for (const auto& value : values) {
        if (!value)
            return std::nullopt;
        resolved.push_back(*value);
    }
    return resolved;
}

DiscreteGridResult evalDiscreteGrid(const DiscreteGridDef& grid,
                                    const std::vector<float>& axisValues,
                                    const VariableLookup& variables)
{
    std::vector<int> reads(grid.axes.size(), 0);
    std::vector<bool> hasRead(grid.axes.size(), false);
    for (size_t a = 0; a < grid.axes.size() && a < axisValues.size(); ++a) {
        reads[a] = VariableSemantics::discreteRead(axisValues[a]);
        hasRead[a] = true;
    }

    DiscreteGridResult result;
    const int axisCount = static_cast<int>(grid.axes.size());
    for (int r = 0; r < static_cast<int>(grid.rows.size()); ++r) {
        bool hit = true;
        for (int a = 0; a < axisCount; ++a) {
            const auto cell = grid.cellAt(r, a);
            if (!cell) continue;
            const auto* variable = variables ? variables(grid.axes[a]) : nullptr;
            const auto value = discreteCellValue(*cell, variable);
            if (!hasRead[a] || !value || reads[a] != *value
                || (variable && !cell->labelId.isNull()
                    && (*value < variable->rangeMin || *value > variable->rangeMax))) {
                hit = false;
                break;
            }
        }
        if (!hit) continue;
        result.hits.push_back(r);
        if (result.winner < 0) {
            result.winner = r;
            continue;
        }
        for (int a = 0; a < axisCount; ++a) {
            const bool championPrecise = grid.cellAt(result.winner, a).has_value();
            const bool contenderPrecise = grid.cellAt(r, a).has_value();
            if (championPrecise == contenderPrecise) continue;
            if (contenderPrecise) result.winner = r;
            break;
        }
    }
    return result;
}

std::vector<GridLintFinding> lintDiscreteGrid(const DiscreteGridDef& grid)
{
    std::vector<GridLintFinding> findings;
    const int axisCount = static_cast<int>(grid.axes.size());
    for (int r = 1; r < static_cast<int>(grid.rows.size()); ++r) {
        for (int earlier = 0; earlier < r; ++earlier) {
            bool identical = true;
            for (int a = 0; a < axisCount; ++a) {
                if (grid.cellAt(earlier, a) != grid.cellAt(r, a)) {
                    identical = false;
                    break;
                }
            }
            if (!identical) continue;
            findings.push_back({r, earlier,
                                grid.rows[earlier].target == grid.rows[r].target
                                    ? GridLintKind::DuplicateRow
                                    : GridLintKind::UnreachableRow});
            break;
        }
    }
    return findings;
}

GridMountState gridMountState(const DiscreteGridDef& grid, VariableResponse response)
{
    if (grid.empty())
        return GridMountState::None;
    return response == VariableResponse::Live
        ? GridMountState::Editable
        : GridMountState::Idle;
}

bool isGridTargetCandidate(const ProjectItem& owner, const QUuid& target)
{
    return owner.type == ProjectItem::Type::Container
        && validateDirectPlaybackTarget(owner, target, findDirectPlaybackChild(owner, target));
}

namespace {

template <typename Grid>
bool remapGridRowTargets(Grid& grid, const QHash<QUuid, QUuid>& guidMap)
{
    bool mapped = false;
    for (auto& row : grid.rows) {
        const auto it = guidMap.constFind(row.target);
        if (it == guidMap.cend()) continue;
        row.target = it.value();
        mapped = true;
    }
    return mapped;
}

template <typename Grid>
bool validateGridRowTargets(const ProjectItem& owner, const Grid& grid,
                            const char* kind, QString* error)
{
    for (size_t i = 0; i < grid.rows.size(); ++i) {
        const QUuid& target = grid.rows[i].target;
        if (isGridTargetCandidate(owner, target)) continue;
        if (error) {
            *error = QStringLiteral(
                "Container '%1' %2 grid row %3 target %4 is not a direct child")
                .arg(owner.name, QLatin1String(kind))
                .arg(i)
                .arg(target.toString(QUuid::WithoutBraces));
        }
        return false;
    }
    return true;
}

}

bool remapGridTargets(DiscreteGridDef& grid, const QHash<QUuid, QUuid>& guidMap)
{
    return remapGridRowTargets(grid, guidMap);
}

bool validateOwnedGridTargets(const ProjectItem& owner, QString* error)
{
    const auto* selection = owner.comp<VSelection>();
    if (!selection) return true;
    for (const auto* table : selection->selections.tables())
        if (!validateGridRowTargets(owner, table->matchGrid(), "discrete", error)
            || !MixingSemantics::validateTargets(owner, table->mixing(), error))
            return false;
    return true;
}

}
}
