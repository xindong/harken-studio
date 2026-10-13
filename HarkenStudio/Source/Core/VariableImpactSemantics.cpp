
#include "VariableImpactSemantics.h"

#include "Components/VAction.h"
#include "Components/VVariable.h"
#include "ProjectTypes.h"

#include <harken_studio_build/VariableExecutionFingerprint.h>
#include <harken_project_schema/VariableContract.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace VNX {
namespace VariableImpactSemantics {

namespace {

bool exactIntegerDeclaration(float value)
{
    return std::isfinite(value) && value == std::floor(value) &&
           value >= -16777216.0f && value <= 16777216.0f;
}

const ProjectItem* findItem(const ProjectItem* item, const QUuid& guid)
{
    if (!item)
        return nullptr;
    if (item->guid == guid)
        return item;
    for (const auto& child : item->children) {
        if (const ProjectItem* found = findItem(child.get(), guid))
            return found;
    }
    return nullptr;
}

const ProjectItem* findItem(const ProjectData* project, const QUuid& guid)
{
    if (!project || guid.isNull())
        return nullptr;
    for (const ProjectFileBinding* binding : project->allFileBindings()) {
        if (binding && binding->rootItem) {
            if (const ProjectItem* found = findItem(binding->rootItem.get(), guid))
                return found;
        }
    }
    return nullptr;
}

harkenstudio::ObjectId itemStableId(const ProjectItem& item)
{
    return item.runtimeId();
}

std::vector<std::int32_t> discreteAnchorsOf(VariableKind kind,
                                            const std::vector<VariableLabel>& labels)
{
    std::vector<std::int32_t> anchors;
    if (kind != VariableKind::Step)
        return anchors;
    anchors.reserve(labels.size());
    for (const auto& label : labels)
        anchors.push_back(label.anchor);
    std::sort(anchors.begin(), anchors.end());
    anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
    return anchors;
}

bool computeExecutionFingerprint(harkenstudio::ObjectId variableId,
                                 const VariableImpactProposed& proposed,
                                 std::uint64_t* outFingerprint)
{
    const auto anchors = discreteAnchorsOf(proposed.kind,
        VariableSemantics::effectiveLabels(proposed.labels, proposed.rangeMin, proposed.rangeMax));
    const std::int32_t* pointer = anchors.empty() ? nullptr : anchors.data();
    return harkenstudiobuild::ComputeVariableExecutionFingerprintV1(
        variableId, static_cast<std::uint8_t>(proposed.kind),
        1, proposed.writers,
        proposed.rangeMin, proposed.rangeMax, proposed.defaultValue, pointer,
        static_cast<std::uint32_t>(anchors.size()), outFingerprint);
}

harkenprojectschema::VariableCatalogBlockV1 catalogBlock(
    const VariableImpactProposed& proposed, const QString& name)
{
    harkenprojectschema::VariableCatalogBlockV1 catalog;
    catalog.codeName = name.toStdString();
    catalog.displayName = name.toStdString();
    catalog.description = proposed.description.toStdString();
    catalog.unit = proposed.unit.toStdString();
    catalog.precision = proposed.precision;
    catalog.displayScale = proposed.displayScale;
    catalog.labels.reserve(proposed.labels.size());
    for (const auto& label : VariableSemantics::effectiveLabels(
             proposed.labels, proposed.rangeMin, proposed.rangeMax)) {
        catalog.labels.push_back(
            {label.anchor, label.name.toStdString(), runtimeObjectId(label.id)});
    }
    std::sort(catalog.labels.begin(), catalog.labels.end(),
              [](const harkenprojectschema::VariableCatalogLabelV1& a,
                 const harkenprojectschema::VariableCatalogLabelV1& b) {
                  return a.anchor < b.anchor;
              });
    return catalog;
}

bool valueInvalidForProposed(float value, const VariableImpactProposed& proposed)
{
    if (!std::isfinite(value) || !std::isfinite(proposed.rangeMin) ||
        !std::isfinite(proposed.rangeMax) || value < proposed.rangeMin ||
        value > proposed.rangeMax)
        return true;
    if (proposed.kind == VariableKind::Step && !exactIntegerDeclaration(value))
        return true;
    return false;
}

}

VariableImpactProposed snapshotOf(const ProjectItem& item)
{
    VariableImpactProposed proposed;
    const auto* source = item.comp<VVariable>();
    if (!source)
        return proposed;
    proposed.kind = source->kind;
    proposed.rangeMin = source->rangeMin;
    proposed.rangeMax = source->rangeMax;
    proposed.defaultValue = source->defaultValue;
    proposed.writers = source->writers;
    proposed.unit = source->unit;
    proposed.precision = source->precision;
    proposed.displayScale = source->displayScale;
    proposed.description = source->description;
    proposed.labels = source->labels;
    return proposed;
}

VariableImpactReport analyzeChange(const ProjectData* project,
                                   const QUuid& variableId,
                                   const VariableImpactProposed& proposed)
{
    VariableImpactReport report;
    const ProjectItem* item = findItem(project, variableId);
    if (!item || item->type != ProjectItem::Type::Variable ||
        !item->comp<VVariable>())
        return report;

    report.sdkSymbol = item->name;
    report.consumers =
        VariableConsumerSemantics::consumersOf(project, variableId);

    const VariableImpactProposed current = snapshotOf(*item);
    const harkenstudio::ObjectId variableStableId = itemStableId(*item);
    const QString currentName = item->name;
    const QString proposedName = proposed.name.value_or(currentName);

    const bool currentExecutionOk = computeExecutionFingerprint(
        variableStableId, current, &report.currentExecutionFingerprint);
    if (!currentExecutionOk)
        report.currentExecutionFingerprint = 0;
    const bool proposedExecutionOk = computeExecutionFingerprint(
        variableStableId, proposed, &report.proposedExecutionFingerprint);
    if (!proposedExecutionOk)
        report.proposedExecutionFingerprint = 0;
    report.executionFingerprintChanged =
        !proposedExecutionOk ||
        report.currentExecutionFingerprint != report.proposedExecutionFingerprint;

    report.currentCatalogFingerprint =
        harkenprojectschema::ComputeVariableCatalogFingerprintV1(
            variableStableId, catalogBlock(current, currentName));
    report.proposedCatalogFingerprint =
        harkenprojectschema::ComputeVariableCatalogFingerprintV1(
            variableStableId, catalogBlock(proposed, proposedName));
    report.catalogFingerprintChanged =
        report.currentCatalogFingerprint != report.proposedCatalogFingerprint;

    report.labelMappingChanged =
        harkenstudiobuild::ComputeVariableLabelMappingFingerprintV1(
            variableStableId, catalogBlock(current, currentName).labels) !=
        harkenstudiobuild::ComputeVariableLabelMappingFingerprintV1(
            variableStableId, catalogBlock(proposed, proposedName).labels);

    report.liveBankResets = report.executionFingerprintChanged;
    report.packMustRebuild = report.executionFingerprintChanged || report.catalogFingerprintChanged;
    report.sdkCatalogMustRebuild =
        report.executionFingerprintChanged || report.catalogFingerprintChanged;

    if (valueInvalidForProposed(proposed.defaultValue, proposed)) {
        VariableImpactInvalidValue invalid;
        invalid.kind = VariableImpactInvalidKind::Default;
        invalid.hostGuid = item->guid;
        invalid.value = proposed.defaultValue;
        report.invalidValues.push_back(invalid);
    }

    std::set<QUuid> seenActions;
    for (const VariableConsumerRef& consumer : report.consumers) {
        if (consumer.kind != VariableConsumerKind::ActionSetVariable)
            continue;
        if (!seenActions.insert(consumer.hostGuid).second)
            continue;
        const ProjectItem* host = findItem(project, consumer.hostGuid);
        const auto* action = host ? host->comp<VAction>() : nullptr;
        if (!action)
            continue;
        for (const VActionStep& step : action->steps) {
            if (step.type != VActionStep::Type::SetVariable ||
                step.targetObjectID != item->objectID)
                continue;
            if (!valueInvalidForProposed(step.value, proposed))
                continue;
            VariableImpactInvalidValue invalid;
            invalid.kind = VariableImpactInvalidKind::ActionSetVariable;
            invalid.hostGuid = consumer.hostGuid;
            invalid.value = step.value;
            report.invalidValues.push_back(invalid);
        }
    }

    for (const InvalidDiscreteGridCell& cell :
         VariableConsumerSemantics::invalidDiscreteGridCells(
             project, variableId, proposed.rangeMin, proposed.rangeMax, &proposed.labels)) {
        VariableImpactInvalidValue invalid;
        invalid.kind = VariableImpactInvalidKind::DiscreteGridCell;
        invalid.hostGuid = cell.hostGuid;
        invalid.value = static_cast<float>(cell.value);
        invalid.rowIndex = cell.rowIndex;
        invalid.axisIndex = cell.axisIndex;
        report.invalidValues.push_back(invalid);
    }

    return report;
}

}
}
