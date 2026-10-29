
#include "VariableConsumerSemantics.h"

#include "VariableConsumerSemanticsPrivate.h"
#include "ActionStepUtils.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "ItemTypeCatalog.h"
#include "FieldGroupProjectAdapter.h"
#include "ProjectTypes.h"
#include "ReferenceSlotResolution.h"
#include "VariableSemantics.h"

#include <QSet>

#include <algorithm>
#include <functional>
#include <optional>

namespace VNX {
namespace VariableConsumerSemantics {

namespace {

void visitItem(const ProjectItem* item, const QUuid& variableId,
               uint32_t variableObjectId,
               std::vector<VariableConsumerRef>& out)
{
    if (!item || variableId.isNull())
        return;

    if (const auto* action = item->comp<VAction>();
        action && variableObjectId != 0) {
        for (const auto& step : action->steps) {
            if (step.type != VActionStep::Type::SetVariable
                || step.targetObjectID != variableObjectId)
                continue;
            VariableConsumerRef ref;
            ref.kind = VariableConsumerKind::ActionSetVariable;
            ref.hostGuid = item->guid;
            out.push_back(ref);
        }
    }

    if (const auto* mix = item->comp<VMixNode>()) {
        for (const ParameterBindingDef& binding : mix->bindings.entries()) {
            if (binding.variable != variableId)
                continue;
            VariableConsumerRef ref;
            ref.kind = VariableConsumerKind::Binding;
            ref.hostGuid = item->guid;
            ref.address = binding.address;
            ref.bindingStableId = binding.runtimeId();
            out.push_back(ref);
        }
    }

    if (const auto* selection = item->comp<VSelection>()) {
        const auto collect = [&](const SelectionTable& table, PlatformScope platform) {
            const auto axis = [&](const QUuid& variable, size_t index, VariableConsumerKind kind) {
                if (variable != variableId) return;
                VariableConsumerRef ref;
                ref.kind = kind;
                ref.hostGuid = item->guid;
                ref.axisIndex = static_cast<int>(index);
                ref.platform = platform;
                out.push_back(ref);
            };
            for (size_t i = 0; i < table.matchAxes.size(); ++i)
                axis(table.matchAxes[i], i, VariableConsumerKind::DiscreteGridAxis);
            for (size_t i = 0; i < table.mixingRows.size(); ++i)
                axis(table.mixingRows[i].variable, i, VariableConsumerKind::ContinuousGridAxis);
        };
        collect(selection->selections.shared, std::nullopt);
        for (auto it = selection->selections.platforms.cbegin(); it != selection->selections.platforms.cend(); ++it)
            collect(it.value(), QUuid(it.key()));
    }

    for (const auto& child : item->children)
        visitItem(child.get(), variableId, variableObjectId, out);
}

ProjectItem* findItemInTree(ProjectItem* item, const QUuid& guid)
{
    if (!item)
        return nullptr;
    if (item->guid == guid)
        return item;
    for (auto& child : item->children) {
        if (ProjectItem* found = findItemInTree(child.get(), guid))
            return found;
    }
    return nullptr;
}

const ProjectItem* findItemInTree(const ProjectItem* item, const QUuid& guid)
{
    return findItemInTree(const_cast<ProjectItem*>(item), guid);
}

}

namespace Internal {

ProjectItem* findItem(ProjectData* project, const QUuid& guid)
{
    if (!project || guid.isNull())
        return nullptr;
    for (ProjectFileBinding* binding : project->allFileBindings()) {
        if (binding && binding->rootItem) {
            if (ProjectItem* found = findItemInTree(binding->rootItem.get(), guid))
                return found;
        }
    }
    return nullptr;
}

const ProjectItem* findItem(const ProjectData* project, const QUuid& guid)
{
    return findItem(const_cast<ProjectData*>(project), guid);
}

}

std::vector<VariableConsumerRef> consumersOf(
    const ProjectData* project, const QUuid& variableId)
{
    std::vector<VariableConsumerRef> out;
    if (!project || variableId.isNull())
        return out;

    const ProjectItem* variable = Internal::findItem(project, variableId);
    const uint32_t variableObjectId = variable ? variable->objectID : 0;

    std::function<void(const ProjectItem*)> visit =
        [&](const ProjectItem* item) {
            visitItem(item, variableId, variableObjectId, out);
        };

    for (const ProjectFileBinding* binding : project->allFileBindings()) {
        if (binding && binding->rootItem)
            visit(binding->rootItem.get());
    }
    return out;
}

AuditionVariableDependencies dependenciesForAudition(
    const ProjectData* project, const ProjectItem* root, FieldGroupScope platform)
{
    AuditionVariableDependencies result;
    const PlatformScope selectionScope = platform ? PlatformScope{fromFieldGroupGuid(*platform)} : std::nullopt;
    if (!project || !root
        || (!ItemTypeCatalog::hasMixingCapability(root->type)
            && root->type != ProjectItem::Type::Action))
        return result;

    QHash<uint32_t, const ProjectItem*> byObject;
    QHash<QUuid, const ProjectItem*> byGuid;
    const auto index = [&](const ProjectItem* item, const auto& self) -> void {
        if (!item || byGuid.contains(item->guid)) return;
        byObject.insert(item->objectID, item);
        byGuid.insert(item->guid, item);
        for (const auto& child : item->children)
            self(child.get(), self);
    };
    index(project->root.get(), index);
    for (const auto* file : project->allFileBindings())
        if (file) index(file->rootItem.get(), index);

    const auto groups = std::make_shared<FieldGroupEffectiveSnapshot>(
        buildFieldGroupEffectiveSnapshot(*project, platform));
    const auto actionLookup = EditorUtils::makeProjectTargetLookup(*project);
    QSet<const ProjectItem*> visited;
    QSet<const ProjectItem*> bindingHosts;
    const auto visit = [&](const ProjectItem* item, const auto& self) -> void {
        if (!item || visited.contains(item)) return;
        visited.insert(item);
        if (item->comp<VVariable>()) {
            result.variables.push_back(item);
            return;
        }
        if (const auto* action = item->comp<VAction>()) {
            for (const auto& step : action->steps) {
                const auto target = EditorUtils::resolveActionStepTarget(
                    step.type, step.targetObjectID, step.targetName,
                    actionLookup, item->objectID);
                if (target.state == EditorUtils::ActionTargetState::Resolved)
                    self(byObject.value(target.objectID), self);
                if ((step.type == VActionStep::Type::Play
                     || step.type == VActionStep::Type::Stop
                     || step.type == VActionStep::Type::PostAction)
                    && step.quantize.timing != QuantizeTiming::Immediate) {
                    const auto reference = EditorUtils::resolveActionStepTarget(
                        VActionStep::Type::Play, step.quantize.referenceObjectID,
                        step.quantize.referenceName, actionLookup);
                    if (reference.state == EditorUtils::ActionTargetState::Resolved)
                        self(byObject.value(reference.objectID), self);
                }
            }
            return;
        }
        if (!ItemTypeCatalog::hasMixingCapability(item->type)) return;

        const auto scope = ReferenceSlotResolution::buildResolutionScope(item, groups);
        const auto hostClass = BindingContractSemantics::hostClassOf(item->type);
        for (const auto& candidate : scope.candidates) {
            if (bindingHosts.contains(candidate.item)) continue;
            const auto* mix = static_cast<const VMixNode*>(
                BindingContractSemantics::resolveHostComponent(candidate.item, hostClass));
            if (!mix) continue;
            bindingHosts.insert(candidate.item);
            for (const auto& binding : mix->bindings.entries()) {
                if (!binding.typed || binding.address.hostClass != hostClass) continue;
                const auto* variable = byGuid.value(binding.variable);
                if (!variable || !variable->comp<VVariable>()) continue;
                self(variable, self);
                VariableConsumerRef ref;
                ref.hostGuid = candidate.item->guid;
                ref.address = binding.address;
                ref.bindingStableId = binding.runtimeId();
                result.consumers.push_back({variable->guid, ref});
            }
        }

        if (item->type == ProjectItem::Type::Bus) {
            if (item->parent && item->parent->type == ProjectItem::Type::Bus)
                self(item->parent, self);
        } else {
            self(byObject.value(
                ReferenceSlotResolution::resolveEffectiveOutputBusId(scope)), self);
        }
        for (const auto& send : ReferenceSlotResolution::resolveEffectiveSends(scope).sends)
            self(byObject.value(send.targetBusID), self);
        if (item->type == ProjectItem::Type::Bus) return;

        for (const auto& child : item->children)
            if (child && ItemTypeCatalog::hasMixingCapability(child->type))
                self(child.get(), self);
        if (const auto* selection = item->comp<VSelection>())
            for (const auto& occurrence : selection->activeRows(selectionScope))
                self(byGuid.value(occurrence.childGuid), self);
        if (const auto* track = item->comp<VTrack>())
            for (const auto& clip : track->clips.items)
                self(byGuid.value(clip.targetGuid), self);
        if (const auto* selection = item->comp<VSelection>()) {
            if (selection->mode == SelectionMode::Match || selection->mode == SelectionMode::Blend) {
                const auto collectAxes = [&](const auto& axes, VariableConsumerKind kind) {
                    for (size_t i = 0; i < axes.size(); ++i) {
                        if (axes[i].isNull()) continue;
                        const ProjectItem* variable = byGuid.value(axes[i]);
                        if (!variable || !variable->comp<VVariable>()) continue;
                        self(variable, self);
                        VariableConsumerRef ref;
                        ref.kind = kind;
                        ref.hostGuid = item->guid;
                        ref.axisIndex = static_cast<int>(i);
                        ref.platform = selectionScope;
                        result.consumers.push_back({variable->guid, ref});
                    }
                };
                if (selection->mode == SelectionMode::Match) {
                    collectAxes(selection->selections.table(selectionScope).matchAxes, VariableConsumerKind::DiscreteGridAxis);
                    for (const auto& row : selection->matchGrid(selectionScope).rows)
                        self(byGuid.value(row.target), self);
                } else {
                    std::vector<QUuid> variables;
                    for (const auto& row : selection->mixing(selectionScope).rows) {
                        variables.push_back(row.variable);
                        for (const auto& child : row.children) self(byGuid.value(child.target), self);
                    }
                    collectAxes(variables, VariableConsumerKind::ContinuousGridAxis);
                }
            }
            if (selection->isQueue()
                || (selection->mode == SelectionMode::Match
                    && selection->variableResponse() == VariableResponse::Live))
                for (const auto& rule : selection->transitions.rules)
                    if (rule.bridge)
                        self(byGuid.value(*rule.bridge), self);
        }
    };
    visit(root, visit);
    const auto compareItems = [](const ProjectItem* a, const ProjectItem* b) {
        const int order = a->name.localeAwareCompare(b->name);
        return order == 0 ? a->guid < b->guid : order < 0;
    };
    std::sort(result.variables.begin(), result.variables.end(), compareItems);
    std::stable_sort(result.consumers.begin(), result.consumers.end(),
        [&](const AuditionVariableConsumer& a, const AuditionVariableConsumer& b) {
            if (a.variableGuid != b.variableGuid)
                return compareItems(byGuid.value(a.variableGuid), byGuid.value(b.variableGuid));
            if (a.consumer.hostGuid != b.consumer.hostGuid)
                return compareItems(byGuid.value(a.consumer.hostGuid), byGuid.value(b.consumer.hostGuid));
            if (a.consumer.kind != b.consumer.kind)
                return a.consumer.kind < b.consumer.kind;
            return a.consumer.address.slotId < b.consumer.address.slotId;
        });
    return result;
}

std::vector<InvalidDiscreteGridCell> invalidDiscreteGridCells(
    const ProjectData* project, const QUuid& variableId,
    float rangeMin, float rangeMax,
    const std::vector<VariableLabel>* proposedLabels)
{
    std::vector<InvalidDiscreteGridCell> out;
    const auto* variableItem = Internal::findItem(project, variableId);
    const auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
    for (const VariableConsumerRef& consumer : consumersOf(project, variableId)) {
        if (consumer.kind != VariableConsumerKind::DiscreteGridAxis)
            continue;
        const ProjectItem* host = Internal::findItem(project, consumer.hostGuid);
        const auto* selection = host ? host->comp<VSelection>() : nullptr;
        if (!selection || consumer.axisIndex < 0)
            continue;
        for (int row = 0; row < static_cast<int>(selection->matchGrid(consumer.platform).rows.size()); ++row) {
            const auto cell =
                selection->matchGrid(consumer.platform).cellAt(row, consumer.axisIndex);
            if (!cell) continue;
            auto value = GridSemantics::discreteCellValue(*cell, variable);
            if (proposedLabels && !cell->labelId.isNull()) {
                const auto* label = VariableSemantics::labelById(*proposedLabels, cell->labelId);
                value = label ? std::optional<int>(label->anchor) : std::nullopt;
            }
            if (!value || *value < rangeMin || *value > rangeMax) {
                out.push_back({consumer.hostGuid, row, consumer.axisIndex, value.value_or(0), consumer.platform});
            }
        }
    }
    return out;
}

}
}
