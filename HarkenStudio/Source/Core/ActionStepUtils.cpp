#include "ActionStepUtils.h"
#include "ItemTypeCatalog.h"

#include <algorithm>
#include <functional>

namespace VNX::EditorUtils {

namespace {

QString leafOf(const QString& name)
{
    const int lastSlash = name.lastIndexOf('/');
    return lastSlash >= 0 ? name.mid(lastSlash + 1) : name;
}

}

const std::vector<ActionStepCatalogEntry>& actionStepCatalog()
{
    using PT = ProjectItem::Type;
    using ET = VActionStep::Type;

    static const std::vector<ActionStepCatalogEntry> catalog = {
        {ET::Play, QStringLiteral("Play"),
         {PT::Sound, PT::Container, PT::Track, PT::Piece},
         true, false, false, false},
        {ET::Stop, QStringLiteral("Stop"),
         {PT::Sound, PT::Container, PT::Track, PT::Piece},
         true, true, false, false},
        {ET::SetVariable, QStringLiteral("Set Variable"),
         {PT::Variable}, false, false, true, false},
        {ET::PostAction, QStringLiteral("Post Action"),
         {PT::Action}, true, false, false, false},
        {ET::Reset, QStringLiteral("Reset"),
         {PT::Container}, false, false, false, true},
    };
    return catalog;
}

const ActionStepCatalogEntry* actionStepCatalogEntry(VActionStep::Type type)
{
    const auto& catalog = actionStepCatalog();
    const auto it = std::find_if(catalog.begin(), catalog.end(),
        [type](const ActionStepCatalogEntry& entry) { return entry.type == type; });
    return it == catalog.end() ? nullptr : &*it;
}

const std::vector<ProjectItem::Type>& actionStepTargetTypes(VActionStep::Type type)
{
    static const std::vector<ProjectItem::Type> empty;
    const ActionStepCatalogEntry* entry = actionStepCatalogEntry(type);
    return entry ? entry->targetTypes : empty;
}

std::vector<ProjectItem::Type> userFacingActionStepTargetTypes(VActionStep::Type type)
{
    std::vector<ProjectItem::Type> out;
    for (ProjectItem::Type target : actionStepTargetTypes(type)) {
        if (ItemTypeCatalog::isUserFacingType(target))
            out.push_back(target);
    }
    return out;
}

bool canActionStepReferenceObject(VActionStep::Type type,
                                  ProjectItem::Type targetType,
                                  uint32_t targetObjectID,
                                  uint32_t currentActionObjectID)
{
    const std::vector<ProjectItem::Type>& valid = actionStepTargetTypes(type);
    if (valid.empty())
        return false;
    if (type == VActionStep::Type::PostAction
        && targetObjectID != 0
        && targetObjectID == currentActionObjectID) {
        return false;
    }
    return std::find(valid.begin(), valid.end(), targetType) != valid.end();
}

bool defaultActionStepTypeForTarget(ProjectItem::Type targetType,
                                    VActionStep::Type& outType)
{
    using PT = ProjectItem::Type;
    using ET = VActionStep::Type;

    switch (targetType) {
        case PT::Action:
            outType = ET::PostAction;
            return true;
        case PT::Variable:
            outType = ET::SetVariable;
            return true;
        case PT::Sound:
        case PT::Container:
        case PT::Track:
        case PT::Piece:
            outType = ET::Play;
            return true;
        default:
            return false;
    }
}

bool isCanonicalActionQuantize(const ActionQuantizeDef& quantize)
{
    return quantize.referenceObjectID == 0
        && quantize.referenceName.isEmpty()
        && quantize.timing == QuantizeTiming::Immediate
        && quantize.ifSilent == QuantizeIfSilent::Immediate;
}

ActionRuntimeReadiness actionStepRuntimeReadiness(const VActionStep& step)
{
    if (!actionStepCatalogEntry(step.type))
        return {ActionRuntimeReadinessState::UnknownType,
                QStringLiteral("Unknown action step type")};
    return {};
}

ActionTargetLookup makeProjectTargetLookup(const ProjectData& project)
{
    ActionTargetLookup lookup;
    std::function<void(const ProjectItem*)> visit = [&](const ProjectItem* item) {
        if (!item) return;
        lookup.add(*item);
        for (const auto& child : item->children)
            visit(child.get());
    };
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem)
            visit(binding->rootItem.get());
    return lookup;
}

void ActionTargetLookup::add(const ProjectItem& item)
{
    const ActionTargetRecord record{item.objectID, item.type, item.name};
    byObjectID_.insert(record.objectID, record);
    byLeafName_[record.name].push_back(record);
}

const ActionTargetRecord* ActionTargetLookup::findByObjectID(uint32_t objectID) const
{
    const auto it = byObjectID_.constFind(objectID);
    return it == byObjectID_.cend() ? nullptr : &it.value();
}

const std::vector<ActionTargetRecord>& ActionTargetLookup::findByLeafName(
    const QString& leafName) const
{
    static const std::vector<ActionTargetRecord> empty;
    const auto it = byLeafName_.constFind(leafName);
    return it == byLeafName_.cend() ? empty : it.value();
}

ActionStepTargetResolution resolveActionStepTarget(
    VActionStep::Type stepType,
    uint32_t targetObjectID,
    const QString& targetName,
    const ActionTargetLookup& lookup,
    uint32_t currentActionObjectID)
{
    ActionStepTargetResolution res;
    res.displayName = targetName;

    if (targetObjectID != 0) {
        if (const ActionTargetRecord* item = lookup.findByObjectID(targetObjectID)) {
            if (canActionStepReferenceObject(stepType, item->type, item->objectID,
                                             currentActionObjectID)) {
                res.state = ActionTargetState::Resolved;
                res.objectID = item->objectID;
                res.displayName = item->name;
                return res;
            }
        }
        res.state = ActionTargetState::Dangling;
        return res;
    }

    if (!targetName.isEmpty()) {
        std::vector<const ActionTargetRecord*> compatible;
        for (const ActionTargetRecord& candidate : lookup.findByLeafName(leafOf(targetName))) {
            if (canActionStepReferenceObject(stepType, candidate.type,
                                             candidate.objectID, currentActionObjectID)) {
                compatible.push_back(&candidate);
            }
        }

        if (compatible.size() == 1) {
            res.state = ActionTargetState::Resolved;
            res.objectID = compatible.front()->objectID;
            res.displayName = compatible.front()->name;
            return res;
        }
        if (compatible.size() > 1) {
            res.state = ActionTargetState::Dangling;
            res.ambiguous = true;
            return res;
        }
    }

    if (targetObjectID == 0 && targetName.isEmpty()) {
        res.state = ActionTargetState::Empty;
        return res;
    }

    res.state = ActionTargetState::Dangling;
    return res;
}

void applyActionStepTypeChange(VActionStep& step,
                               VActionStep::Type newType,
                               const ActionTargetLookup& lookup,
                               uint32_t currentActionObjectID)
{
    const ActionStepTargetResolution current = resolveActionStepTarget(
        step.type, step.targetObjectID, step.targetName, lookup,
        currentActionObjectID);

    const ActionStepCatalogEntry* oldEntry = actionStepCatalogEntry(step.type);
    const ActionStepCatalogEntry* newEntry = actionStepCatalogEntry(newType);
    const bool oldQuantized = oldEntry && oldEntry->supportsQuantize;
    const bool newQuantized = newEntry && newEntry->supportsQuantize;

    step.type = newType;

    if (!newQuantized || !oldQuantized)
        step.quantize = {};

    if (newType != VActionStep::Type::Stop)
        step.fadeTime = 0.0f;
    if (newType != VActionStep::Type::SetVariable)
        step.value = 0.0f;
    if (newType != VActionStep::Type::Reset)
        step.resetScope = ResetScope::Emitter;

    if (current.state != ActionTargetState::Resolved)
        return;

    const ActionTargetRecord* target = lookup.findByObjectID(current.objectID);
    if (target && canActionStepReferenceObject(newType, target->type,
                                               target->objectID,
                                               currentActionObjectID)) {
        step.targetObjectID = target->objectID;
        step.targetName = target->name;
        return;
    }

    step.targetObjectID = 0;
    step.targetName.clear();
}

}
