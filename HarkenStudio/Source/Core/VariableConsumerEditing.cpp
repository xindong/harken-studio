
#include "VariableConsumerSemantics.h"

#include "BindingEditing.h"
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

#include "VariableConsumerSemanticsPrivate.h"

namespace harken::studio::VariableConsumerSemantics {

bool retarget(ProjectData* project, const QUuid& fromGuid, const QUuid& toGuid)
{
    if (!project || fromGuid.isNull() || toGuid.isNull())
        return false;
    ProjectItem* to = Internal::findItem(project, toGuid);
    if (!to || to->type != ProjectItem::Type::Variable)
        return false;
    const ProjectItem* from = Internal::findItem(project, fromGuid);
    const uint32_t fromObjectId = from ? from->objectID : 0;
    const uint32_t toObjectId = to->objectID;
    const auto consumers = consumersOf(project, fromGuid);
    const auto* targetVariable = to->comp<VVariable>();
    for (const auto& consumer : consumers) {
        if (consumer.kind == VariableConsumerKind::ContinuousGridAxis) {
            const auto* host = Internal::findItem(project, consumer.hostGuid);
            const auto* selection = host ? host->comp<VSelection>() : nullptr;
            if (!selection) return false;
            const int existing = selection->mixing(consumer.platform).rowIndexForVariable(toGuid);
            if (existing >= 0 && existing != consumer.axisIndex) return false;
        }
        if (consumer.kind != VariableConsumerKind::DiscreteGridAxis) continue;
        const auto* host = Internal::findItem(project, consumer.hostGuid);
        const auto* selection = host ? host->comp<VSelection>() : nullptr;
        if (!selection || !targetVariable) return false;
        QList<PlatformScope> scopes{std::nullopt};
        for (const auto& [platform, table] : selection->selections.match.platforms) scopes.append(platform);
        for (const auto& scope : scopes) {
            const auto grid = selection->matchGrid(scope);
            if (grid.axisIndex(toGuid) >= 0 && grid.axisIndex(toGuid) != consumer.axisIndex) return false;
            for (int row = 0; row < static_cast<int>(grid.rows.size()); ++row) {
                const auto cell = grid.cellAt(row, consumer.axisIndex);
                if (cell && !cell->labelId.isNull() &&
                    !VariableSemantics::labelById(targetVariable->labels, cell->labelId)) return false;
            }
        }
    }
    for (const VariableConsumerRef& consumer : consumers) {
        ProjectItem* host = Internal::findItem(project, consumer.hostGuid);
        if (!host)
            return false;
        switch (consumer.kind) {
        case VariableConsumerKind::Binding: {
            auto* mix = host->comp<VMixNode>();
            const ParameterBindingDef* current =
                mix ? mix->bindings.find(consumer.bindingStableId) : nullptr;
            if (!current)
                return false;
            ParameterBindingDef updated = *current;
            updated.variable = toGuid;
            if (!BindingEditing::upsert(host, std::move(updated)))
                return false;
            break;
        }
        case VariableConsumerKind::DiscreteGridAxis: {
            auto* selection = host->comp<VSelection>();
            if (!selection || consumer.axisIndex < 0 ||
                consumer.axisIndex >= static_cast<int>(selection->selections.matchAxes.size()))
                return false;
            selection->selections.replaceMatchAxis(static_cast<size_t>(consumer.axisIndex), toGuid);
            host->markDirty();
            break;
        }
        case VariableConsumerKind::ContinuousGridAxis: {
            auto* selection = host->comp<VSelection>();
            if (!selection || consumer.axisIndex < 0 ||
                consumer.axisIndex >= static_cast<int>(selection->selections.blendAxes.size()))
                return false;
            selection->selections.blendAxes[static_cast<size_t>(consumer.axisIndex)].variable = toGuid;
            host->markDirty();
            break;
        }
        case VariableConsumerKind::ActionSetVariable: {
            auto* action = host->comp<VAction>();
            if (!action || fromObjectId == 0)
                return false;
            bool rewritten = false;
            for (VActionStep& step : action->steps) {
                if (step.type != VActionStep::Type::SetVariable ||
                    step.targetObjectID != fromObjectId)
                    continue;
                step.targetObjectID = toObjectId;
                rewritten = true;
            }
            if (rewritten)
                host->markDirty();
            break;
        }
        }
    }
    return true;
}

bool clear(ProjectData* project, const QUuid& fromGuid)
{
    if (!project || fromGuid.isNull())
        return false;
    const ProjectItem* from = Internal::findItem(project, fromGuid);
    const uint32_t fromObjectId = from ? from->objectID : 0;
    auto consumers = consumersOf(project, fromGuid);
    std::sort(consumers.begin(), consumers.end(),
              [](const VariableConsumerRef& a, const VariableConsumerRef& b) {
                  if (a.hostGuid != b.hostGuid)
                      return a.hostGuid < b.hostGuid;
                  if (a.platform != b.platform) return a.platform < b.platform;
                  if (a.kind != b.kind)
                      return static_cast<uint8_t>(a.kind) <
                             static_cast<uint8_t>(b.kind);
                  return a.axisIndex > b.axisIndex;
              });
    for (const VariableConsumerRef& consumer : consumers) {
        ProjectItem* host = Internal::findItem(project, consumer.hostGuid);
        if (!host)
            return false;
        switch (consumer.kind) {
        case VariableConsumerKind::Binding:
            if (!BindingEditing::remove(host, consumer.bindingStableId))
                return false;
            break;
        case VariableConsumerKind::DiscreteGridAxis: {
            auto* selection = host->comp<VSelection>();
            if (!selection) return false;
            auto grid = selection->selections.matchGrid();
            if (!grid.removeAxis(consumer.axisIndex) || !selection->selections.setMatchGrid(grid, std::nullopt))
                return false;
            host->markDirty();
            break;
        }
        case VariableConsumerKind::ContinuousGridAxis: {
            auto* selection = host->comp<VSelection>();
            if (!selection || consumer.axisIndex < 0
                || consumer.axisIndex >= static_cast<int>(selection->selections.blendAxes.size()))
                return false;
            auto mixing = selection->selections.mixing();
            mixing.rows.erase(mixing.rows.begin() + consumer.axisIndex);
            if (!selection->selections.setMixing(mixing, std::nullopt)) return false;
            host->markDirty();
            break;
        }
        case VariableConsumerKind::ActionSetVariable: {
            auto* action = host->comp<VAction>();
            if (!action || fromObjectId == 0)
                return false;
            const auto before = action->steps.size();
            action->steps.erase(
                std::remove_if(action->steps.begin(), action->steps.end(),
                               [fromObjectId](const VActionStep& step) {
                                   return step.type == VActionStep::Type::SetVariable &&
                                          step.targetObjectID == fromObjectId;
                               }),
                action->steps.end());
            if (action->steps.size() != before)
                host->markDirty();
            break;
        }
        }
    }
    return true;
}

}
