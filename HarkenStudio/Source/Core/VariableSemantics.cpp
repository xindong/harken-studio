
#include "VariableSemantics.h"

#include "Components/VVariable.h"
#include "Components/VSelection.h"
#include "ProjectTypes.h"

#include <QSet>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

namespace VNX {
namespace VariableSemantics {

EditForm editFormForKind(VariableKind kind)
{
    return kind == VariableKind::Step ? EditForm{0, 1.0f} : EditForm{2, 0.0f};
}

int discreteRead(float value)
{
    if (!std::isfinite(value))
        return 0;
    const double rounded = std::floor(static_cast<double>(value) + 0.5);
    constexpr double kMin = static_cast<double>(std::numeric_limits<int>::min());
    constexpr double kMax = static_cast<double>(std::numeric_limits<int>::max());
    if (rounded <= kMin)
        return std::numeric_limits<int>::min();
    if (rounded >= kMax)
        return std::numeric_limits<int>::max();
    return static_cast<int>(rounded);
}

bool discreteCellReachable(int cellValue, float rangeMin, float rangeMax)
{
    if (!std::isfinite(rangeMin) || !std::isfinite(rangeMax))
        return false;
    const int lo = std::min(discreteRead(rangeMin), discreteRead(rangeMax));
    const int hi = std::max(discreteRead(rangeMin), discreteRead(rangeMax));
    return cellValue >= lo && cellValue <= hi;
}

int clampStepExactInt(int value)
{
    if (value < -kStepExactIntBound)
        return -kStepExactIntBound;
    if (value > kStepExactIntBound)
        return kStepExactIntBound;
    return value;
}

bool stepComboFits(float rangeMin, float rangeMax)
{
    const int begin = discreteRead(rangeMin);
    const int end = discreteRead(rangeMax);
    const int lo = std::min(begin, end);
    const int hi = std::max(begin, end);
    if (lo < -kStepExactIntBound || hi > kStepExactIntBound)
        return false;
    const int64_t count = static_cast<int64_t>(hi) - lo + 1;
    return count > 0 && count <= kStepComboMaxInclusiveCount;
}

StepExactSpinRange stepExactSpinRange(float rangeMin, float rangeMax)
{
    const int lo = std::min(discreteRead(rangeMin), discreteRead(rangeMax));
    const int hi = std::max(discreteRead(rangeMin), discreteRead(rangeMax));
    const int begin = std::max(lo, -kStepExactIntBound);
    const int end = std::min(hi, kStepExactIntBound);
    if (begin > end)
        return {};
    return {begin, end, true};
}

std::optional<QString> labelTextForValue(float value, const std::vector<VariableLabel>& labels)
{
    const int anchor = discreteRead(value);
    for (const VariableLabel& label : labels) {
        if (label.anchor == anchor && !label.name.isEmpty()) return label.name;
    }
    return std::nullopt;
}

const VariableLabel* labelAtIndex(const std::vector<VariableLabel>& labels, int index)
{
    const auto found = std::find_if(labels.begin(), labels.end(),
        [index](const VariableLabel& label) { return label.anchor == index; });
    return found == labels.end() ? nullptr : &*found;
}

const VariableLabel* labelById(const std::vector<VariableLabel>& labels, const QUuid& id)
{
    if (id.isNull()) return nullptr;
    const auto found = std::find_if(labels.begin(), labels.end(),
        [&id](const VariableLabel& label) { return label.id == id; });
    return found == labels.end() ? nullptr : &*found;
}

QString labelNameError(const QString& name, const QSet<QString>& otherNames)
{
    if (name.isEmpty()) return {};
    if (name != name.trimmed())
        return QStringLiteral("Label name has surrounding whitespace");
    if (name.contains(QChar(u'\0')))
        return QStringLiteral("Label name contains a null character");
    static const QRegularExpression numericName(
        QStringLiteral(R"(^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$)"));
    if (numericName.match(name).hasMatch())
        return QStringLiteral("Label name cannot be a number");
    if (otherNames.contains(name))
        return QStringLiteral("Duplicate Label name '%1'").arg(name);
    return {};
}

bool validateLabels(const std::vector<VariableLabel>& labels, QString* error)
{
    QSet<int> indices;
    QSet<QString> names;
    QSet<QUuid> identities;
    for (const auto& label : labels) {
        QString reason;
        if (label.anchor < 0 || label.anchor > kStepExactIntBound)
            reason = QStringLiteral("Label index %1 must be between 0 and %2")
                         .arg(label.anchor).arg(kStepExactIntBound);
        else if (indices.contains(label.anchor))
            reason = QStringLiteral("Duplicate Label index %1").arg(label.anchor);
        else if (label.id.isNull() || identities.contains(label.id))
            reason = QStringLiteral("Invalid or duplicate Label identity at index %1").arg(label.anchor);
        else
            reason = labelNameError(label.name, names);
        if (!reason.isEmpty()) {
            if (error) *error = reason;
            return false;
        }
        indices.insert(label.anchor);
        identities.insert(label.id);
        if (!label.name.isEmpty()) names.insert(label.name);
    }
    return true;
}

std::vector<VariableLabel> effectiveLabels(const VVariable& variable)
{
    return effectiveLabels(variable.labels, variable.rangeMin, variable.rangeMax);
}

std::vector<VariableLabel> effectiveLabels(const std::vector<VariableLabel>& labels,
                                           float rangeMin, float rangeMax)
{
    std::vector<VariableLabel> result;
    for (const auto& label : labels)
        if (label.anchor >= rangeMin && label.anchor <= rangeMax)
            result.push_back(label);
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) { return a.anchor < b.anchor; });
    return result;
}

void initializeNewVariable(VVariable& variable, VariableKind kind)
{
    variable.kind = kind;
    variable.rangeMin = 0.0f;
    variable.rangeMax = kind == VariableKind::Step ? 3.0f : 100.0f;
    variable.defaultValue = 0.0f;
    variable.labels.clear();
    if (kind == VariableKind::Step) {
        variable.labels.push_back({0, QStringLiteral("None")});
        for (int index = 1; index <= static_cast<int>(variable.rangeMax); ++index)
            variable.labels.push_back({index, {}});
    }
}

float clampToRange(float value, float rangeMin, float rangeMax)
{
    if (value < rangeMin) return rangeMin;
    if (value > rangeMax) return rangeMax;
    return value;
}

float resolveDefaultValue(float defaultValue, float rangeMin, float rangeMax)
{
    return clampToRange(defaultValue, rangeMin, rangeMax);
}

float currentValue(std::optional<float> rehearsal, VariableKind kind,
                   float rangeMin, float rangeMax, float defaultValue)
{
    float value = clampToRange(rehearsal.value_or(defaultValue), rangeMin, rangeMax);
    if (kind == VariableKind::Step)
        value = clampToRange(static_cast<float>(discreteRead(value)), rangeMin, rangeMax);
    return value;
}

NormalizedDeclaration normalizeDeclarationForKind(VariableKind kind,
                                                  float rangeMin,
                                                  float rangeMax,
                                                  float defaultValue)
{
    if (kind == VariableKind::Step) {
        rangeMin = static_cast<float>(discreteRead(rangeMin));
        rangeMax = static_cast<float>(discreteRead(rangeMax));
        defaultValue = static_cast<float>(discreteRead(defaultValue));
    }
    return { rangeMin, rangeMax, resolveDefaultValue(defaultValue, rangeMin, rangeMax) };
}

std::vector<ProjectItem*> fileBoundVariables(ProjectData* project)
{
    std::vector<ProjectItem*> variables;
    if (!project) return variables;
    std::function<void(ProjectItem*)> visit = [&](ProjectItem* item) {
        if (!item) return;
        if (item->comp<VVariable>())
            variables.push_back(item);
        for (auto& child : item->children)
            visit(child.get());
    };
    for (ProjectFileBinding* binding : project->allFileBindings())
        if (binding && binding->rootItem)
            visit(binding->rootItem.get());
    std::sort(variables.begin(), variables.end(),
        [](ProjectItem* a, ProjectItem* b) {
            return a->name.localeAwareCompare(b->name) < 0;
        });
    return variables;
}

QStringList restoreLoadedLabelReferences(ProjectData& project)
{
    QStringList diagnostics;
    QHash<QUuid, VVariable*> variables;
    const auto variableItems = fileBoundVariables(&project);
    for (auto* item : variableItems)
        variables.insert(item->guid, item->comp<VVariable>());
    const auto restore = [&](auto&& self, ProjectItem* item) -> void {
        if (!item) return;
        if (auto* selection = item->comp<VSelection>()) {
            for (auto* table : selection->selections.tables()) {
                for (size_t axis = 0; axis < table->matchAxes.size(); ++axis) {
                    auto* variable = variables.value(table->matchAxes[axis], nullptr);
                    for (auto& entry : table->entries) {
                        for (auto& condition : entry.conditions) {
                            if (axis >= condition.cells.size() || !condition.cells[axis]) continue;
                            auto& cell = *condition.cells[axis];
                            if (!cell.labelId.isNull()) continue;
                            const auto matches = variable ? std::count_if(variable->labels.begin(), variable->labels.end(),
                                [&](const auto& label) { return label.anchor == cell.legacyValue; }) : 0;
                            if (!variable || matches > 1) {
                                diagnostics.append(QStringLiteral("Grid '%1' index %2 has a missing or ambiguous Label; its numeric condition is retained")
                                    .arg(item->name).arg(cell.legacyValue));
                                continue;
                            }
                            const auto* label = labelAtIndex(variable->labels, cell.legacyValue);
                            if (!label) {
                                variable->labels.push_back({cell.legacyValue, {}, QUuid::createUuidV5(table->matchAxes[axis],
                                    QByteArray("grid-label-") + QByteArray::number(cell.legacyValue))});
                                label = &variable->labels.back();
                            }
                            cell = DiscreteGridCell(label->id);
                        }
                    }
                }
            }
        }
        for (const auto& child : item->children) self(self, child.get());
    };
    for (const auto* binding : project.allFileBindings())
        if (binding) restore(restore, binding->rootItem.get());
    for (auto* item : variableItems) {
        auto* variable = item->comp<VVariable>();
        QString error;
        if (variable->rangeMin < 0)
            diagnostics.append(QStringLiteral("Variable '%1' has a negative range; original values are retained")
                .arg(item->name));
        if (!validateLabels(variable->labels, &error))
            diagnostics.append(QStringLiteral("Variable '%1': %2").arg(item->name, error));
    }
    return diagnostics;
}

}
}
