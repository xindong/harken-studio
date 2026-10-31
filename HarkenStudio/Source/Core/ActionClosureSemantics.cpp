#include "ActionClosureSemantics.h"

#include "ActionStepUtils.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "SoundSourceSemantics.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "ItemTypeCatalog.h"
#include "ReferenceSlotResolution.h"

#include <QJsonArray>

#include <algorithm>
#include <map>
#include <set>

namespace VNX {
namespace {

QJsonArray pathJson(const std::vector<harkenstudio::ObjectId>& path)
{
    QJsonArray result;
    for (harkenstudio::ObjectId id : path)
        result.append(objectIdString(id));
    return result;
}

QString itemTypeName(ProjectItem::Type type)
{
    return ItemTypeCatalog::canonicalName(type);
}

bool isContentType(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Sound
        || type == ProjectItem::Type::Container
        || type == ProjectItem::Type::Track
        || type == ProjectItem::Type::Segment;
}

struct Index
{
    std::map<harkenstudio::ObjectId, const ProjectItem*> stable;
    std::map<uint32_t, const ProjectItem*> object;
    std::map<QUuid, const ProjectItem*> guid;
    std::set<uint32_t> duplicateObjectIds;
    std::set<QUuid> duplicateGuids;
    std::vector<ActionClosureDiagnostic> diagnostics;
};

void indexItem(const ProjectItem* item, Index& index, std::set<const ProjectItem*>& seen)
{
    if (!item || !seen.insert(item).second)
        return;
    if (harkenstudio::IsNull(item->runtimeId())) {
        index.diagnostics.push_back({
            ActionClosureDiagnosticCode::MissingStableIdentity,
            QStringLiteral("%1 '%2' has no runtimeStableId")
                .arg(itemTypeName(item->type), item->name),
            {}, -1, {}, QStringLiteral("runtimeStableId"), {}});
    } else if (!index.stable.emplace(item->runtimeId(), item).second) {
        index.diagnostics.push_back({
            ActionClosureDiagnosticCode::DuplicateStableIdentity,
            QStringLiteral("Duplicate runtimeStableId %1").arg(objectIdString(item->runtimeId())),
            {}, -1, item->runtimeId(), QStringLiteral("runtimeStableId"), {}});
    }
    if (item->objectID != 0
        && !index.object.emplace(item->objectID, item).second)
        index.duplicateObjectIds.insert(item->objectID);
    if (!item->guid.isNull()
        && !index.guid.emplace(item->guid, item).second)
        index.duplicateGuids.insert(item->guid);
    for (const auto& child : item->children)
        indexItem(child.get(), index, seen);
}

Index buildIndex(const ProjectData& project)
{
    Index index;
    std::set<const ProjectItem*> seen;
    indexItem(project.root.get(), index, seen);
    for (const ProjectFileBinding* binding : project.allFileBindings())
        if (binding)
            indexItem(binding->rootItem.get(), index, seen);
    return index;
}

struct Builder
{
    const ProjectData& project;
    BuildVariant variant;
    RuntimeCapabilityContext capabilityContext;
    bool contentReferencesOnly;
    std::shared_ptr<const Index> indexOwner;
    const Index& index;
    std::shared_ptr<const FieldGroupEffectiveSnapshot> effectiveGroups;
    std::shared_ptr<const EditorUtils::ActionTargetLookup> targetLookupOwner;
    const EditorUtils::ActionTargetLookup& targetLookup;
    ActionClosurePlan plan;
    std::set<harkenstudio::ObjectId> actionVisited;
    std::set<harkenstudio::ObjectId> actionActive;
    std::set<harkenstudio::ObjectId> objectVisited;
    std::set<harkenstudio::ObjectId> controlVisited;
    std::set<std::tuple<harkenstudio::ObjectId, harkenstudio::ObjectId, ActionClosureEdgeReason, QString,
                        std::vector<harkenstudio::ObjectId>>> edgeKeys;
    std::set<std::tuple<RuntimeCapabilityFeature, harkenstudio::ObjectId, QString>> capabilityKeys;

    Builder(const ProjectData& projectValue, BuildVariant variantValue,
            RuntimeCapabilityContext context, bool contentReferencesOnlyValue = false,
            std::shared_ptr<const Index> sharedIndex = {},
            std::shared_ptr<const EditorUtils::ActionTargetLookup> sharedLookup = {})
        : project(projectValue), variant(std::move(variantValue)),
          capabilityContext(std::move(context)),
          contentReferencesOnly(contentReferencesOnlyValue),
          indexOwner(sharedIndex ? std::move(sharedIndex) : std::make_shared<Index>(buildIndex(projectValue))),
          index(*indexOwner),
          targetLookupOwner(sharedLookup ? std::move(sharedLookup)
              : std::make_shared<EditorUtils::ActionTargetLookup>(EditorUtils::makeProjectTargetLookup(projectValue))),
          targetLookup(*targetLookupOwner)
    {
        plan.variant = variant;
        plan.diagnostics = index.diagnostics;
        if (contentReferencesOnly)
            return;
        FieldGroupScope scope;
        if (!variant.platform.isEmpty()) {
            const auto parsed = ProjectPlatformSemantics::parseId(variant.platform);
            if (!parsed) {
                diagnostic(ActionClosureDiagnosticCode::SchemaMismatch,
                    QStringLiteral("Unknown field group target platform"),
                    {}, -1, {}, QStringLiteral("platform"), {});
                return;
            }
            scope = toFieldGroupGuid(*parsed);
        }
        effectiveGroups = capabilityContext.buildSession
            ? capabilityContext.buildSession->fieldGroups(variant.platform.isEmpty()
                ? PlatformScope{} : PlatformScope{QUuid::fromString(variant.platform)})
            : std::make_shared<FieldGroupEffectiveSnapshot>(
                buildFieldGroupEffectiveSnapshot(project, scope));
        for (const auto& error : effectiveGroups->diagnostics) {
            const auto object = index.guid.find(error.itemGuid);
            diagnostic(ActionClosureDiagnosticCode::SchemaMismatch, error.message,
                       {}, -1, object == index.guid.end() ? harkenstudio::ObjectId{} : object->second->runtimeId(),
                       error.field, {});
        }
    }

    void diagnostic(ActionClosureDiagnosticCode code, const QString& message,
                    harkenstudio::ObjectId actionId, int stepIndex, harkenstudio::ObjectId objectId,
                    const QString& field, const std::vector<harkenstudio::ObjectId>& path)
    {
        plan.diagnostics.push_back({code, message, actionId, stepIndex,
                                    objectId, field, path});
    }

    void edge(harkenstudio::ObjectId from, harkenstudio::ObjectId to, ActionClosureEdgeReason reason,
              const QString& field, const std::vector<harkenstudio::ObjectId>& path)
    {
        if (!edgeKeys.emplace(from, to, reason, field, path).second)
            return;
        plan.edges.push_back({from, to, reason, field, path});
    }

    void capability(RuntimeCapabilityFeature feature, RuntimeCapabilityState state,
                    harkenstudio::ObjectId objectId, const QString& field)
    {
        if (contentReferencesOnly || capabilityContext.deferCurrentWorldCompilation)
            return;
        if (!capabilityKeys.emplace(feature, objectId, field).second)
            return;
        QString reason;
        const auto object = index.stable.find(objectId);
        if (object != index.stable.end()) {
            const auto assessment = RuntimeCapabilityMatrix::evaluateForContext(
                *object->second, project, RuntimeCapabilityOperation::PackBuild, feature, variant,
                capabilityContext);
            state = assessment.state;
            reason = assessment.diagnostic;
        }
        plan.capabilities.push_back({feature, state, objectId, field});
        if (state == RuntimeCapabilityState::UnavailableEngineContract
            || state == RuntimeCapabilityState::InvalidAuthoring) {
            diagnostic(ActionClosureDiagnosticCode::UnsupportedCapability,
                       reason.isEmpty()
                           ? QStringLiteral("%1 requires unavailable capability %2")
                                 .arg(field, RuntimeCapabilityMatrix::featureName(feature))
                           : reason,
                       plan.rootActionStableId, -1, objectId, field, {});
        }
    }

    ActionClosureMember member(const ProjectItem& item,
                               const std::vector<harkenstudio::ObjectId>& path) const
    {
        return {item.runtimeId(), item.objectID, item.guid, item.type,
                item.name, path};
    }

    const ProjectItem* byObject(uint32_t objectId, harkenstudio::ObjectId actionId,
                                int stepIndex, const QString& field,
                                const std::vector<harkenstudio::ObjectId>& path)
    {
        if (objectId != 0 && index.duplicateObjectIds.contains(objectId)) {
            diagnostic(ActionClosureDiagnosticCode::SchemaMismatch,
                       QStringLiteral("Object reference %1 is ambiguous").arg(objectId),
                       actionId, stepIndex, {}, field, path);
            return nullptr;
        }
        const auto it = index.object.find(objectId);
        if (objectId == 0 || it == index.object.end()) {
            diagnostic(ActionClosureDiagnosticCode::DanglingReference,
                       QStringLiteral("Reference %1 does not resolve").arg(objectId),
                       actionId, stepIndex, {}, field, path);
            return nullptr;
        }
        return it->second;
    }

    const ProjectItem* byGuid(const QUuid& guid, harkenstudio::ObjectId ownerId,
                              const QString& field,
                              const std::vector<harkenstudio::ObjectId>& path)
    {
        if (!guid.isNull() && index.duplicateGuids.contains(guid)) {
            diagnostic(ActionClosureDiagnosticCode::SchemaMismatch,
                       QStringLiteral("GUID reference '%1' is ambiguous")
                           .arg(guid.toString(QUuid::WithoutBraces)),
                       plan.rootActionStableId, -1, ownerId, field, path);
            return nullptr;
        }
        const auto it = index.guid.find(guid);
        if (guid.isNull() || it == index.guid.end()) {
            diagnostic(ActionClosureDiagnosticCode::DanglingReference,
                       QStringLiteral("GUID reference '%1' does not resolve")
                           .arg(guid.toString(QUuid::WithoutBraces)),
                       plan.rootActionStableId, -1, ownerId, field, path);
            return nullptr;
        }
        return it->second;
    }

    void addDependencies(const ProjectItem& item, const std::vector<harkenstudio::ObjectId>& path)
    {
        if (contentReferencesOnly)
            return;
        const auto scope = ReferenceSlotResolution::buildResolutionScope(&item, effectiveGroups);
        const auto output = ReferenceSlotResolution::resolveOutputBusSlot(scope);
        if (output.target && !harkenstudio::IsNull(output.target->runtimeId()))
            edge(item.runtimeId(), output.target->runtimeId(),
                  ActionClosureEdgeReason::EffectiveOutputBus,
                  QStringLiteral("outputBus"), path);
        const auto attenuation = ReferenceSlotResolution::resolveEffectiveAttenuation(scope);
        if (attenuation.target && !harkenstudio::IsNull(attenuation.target->runtimeId()))
            edge(item.runtimeId(), attenuation.target->runtimeId(),
                  ActionClosureEdgeReason::EffectiveAttenuation,
                  QStringLiteral("attenuation"), path);
        for (const auto& send : ReferenceSlotResolution::resolveEffectiveSends(scope).sends) {
            const auto* target = byObject(send.targetBusID, plan.rootActionStableId, -1,
                                          QStringLiteral("sends.targetBusID"), path);
            if (target)
                edge(item.runtimeId(), target->runtimeId(),
                     ActionClosureEdgeReason::EffectiveSend,
                     QStringLiteral("sends.targetBusID"), path);
        }
        const BindingHostClass hostClass = BindingContractSemantics::hostClassOf(item.type);
        for (const auto& candidate : scope.candidates) {
            const auto* host = static_cast<const VMixNode*>(
                BindingContractSemantics::resolveHostComponent(candidate.item, hostClass));
            if (!host) continue;
            for (const auto& binding : host->bindings.entries()) {
                if (!binding.typed || binding.address.hostClass != hostClass) continue;
                const ProjectItem* variable = byGuid(
                    binding.variable, item.runtimeId(),
                    QStringLiteral("bindings.variable"), path);
                if (!variable) continue;
                edge(item.runtimeId(), variable->runtimeId(),
                     ActionClosureEdgeReason::BindingDependency,
                     QStringLiteral("bindings"), path);
                capability(RuntimeCapabilityFeature::TypedBinding,
                           RuntimeCapabilityState::Ready, item.runtimeId(),
                           QStringLiteral("bindings"));
            }
        }
        if (const auto* mix = item.comp<VMixNode>(); mix && !mix->effects.isEmpty())
            edge(item.runtimeId(), item.runtimeId(),
                 ActionClosureEdgeReason::EffectDependency,
                 QStringLiteral("effects"), path);
    }

    void addSoundMedia(const ProjectItem& item, bool control)
    {
        if (contentReferencesOnly)
            return;
        const auto* sound = item.comp<VSound>();
        if (!sound) {
            diagnostic(ActionClosureDiagnosticCode::SchemaMismatch,
                       QStringLiteral("Sound has no source component"),
                       plan.rootActionStableId, -1, item.runtimeId(),
                       QStringLiteral("sources"), {});
            return;
        }
        if (variant.platform.trimmed().isEmpty()) {
            diagnostic(ActionClosureDiagnosticCode::AmbiguousSourceSelection,
                       QStringLiteral("build platform must not be empty"),
                       plan.rootActionStableId, -1, item.runtimeId(),
                       QStringLiteral("sources"), {});
            return;
        }
        auto add = [&](const QString& language) {
            const auto resolution = SoundSourceSemantics::resolveSourceSelection(
                *sound, variant.platform, language);
            if (!resolution) {
                diagnostic(ActionClosureDiagnosticCode::AmbiguousSourceSelection,
                           resolution.error, plan.rootActionStableId, -1,
                           item.runtimeId(), QStringLiteral("sources"), {});
                return;
            }
            const QString platform = variant.platform;
            const QString artifactLanguage = language.isEmpty()
                ? QStringLiteral("none") : language;
            (control ? plan.controlMedia : plan.media).push_back({item.runtimeId(), resolution.source->entryId,
                                  resolution.source->sourceId, platform, language,
                                  QStringLiteral("%1_%2_%3")
                                      .arg(objectIdString(item.runtimeId())).arg(platform, artifactLanguage)});
        };
        if (!sound->distinguishLanguage)
            add(QString());
        else
            add(variant.language);
    }

    void visitTrack(const ProjectItem& track, const std::vector<harkenstudio::ObjectId>& path,
                      bool control = false)
    {
        const auto* timeline = track.comp<VTrack>();
        if (!timeline) {
            diagnostic(ActionClosureDiagnosticCode::SchemaMismatch,
                       QStringLiteral("Track has no timeline component"),
                       plan.rootActionStableId, -1, track.runtimeId(),
                       QStringLiteral("clips"), path);
            return;
        }
        for (const Clip& clip : timeline->clips.items) {
            const ProjectItem* target = byGuid(clip.targetGuid, track.runtimeId(),
                                               QStringLiteral("clips.targetGuid"), path);
            if (!target)
                continue;
            std::vector<harkenstudio::ObjectId> targetPath = path;
            targetPath.push_back(target->runtimeId());
            edge(track.runtimeId(), target->runtimeId(),
                 ActionClosureEdgeReason::TrackClip,
                 clip.clipGuid.toString(QUuid::WithoutBraces), targetPath);
            visitContent(*target, targetPath, control);
        }
    }

    void visitContainer(const ProjectItem& container, const std::vector<harkenstudio::ObjectId>& path,
                      bool control = false)
    {
        const auto platform = ProjectPlatformSemantics::parseId(variant.platform);
        const auto* selection = container.comp<VSelection>();
        if (!selection) return;
        for (const auto& occurrence : selection->activeRows(platform)) {
            const ProjectItem* child = byGuid(
                occurrence.childGuid, container.runtimeId(),
                QStringLiteral("selections.childGuid"), path);
            if (!child)
                continue;
            std::vector<harkenstudio::ObjectId> childPath = path;
            childPath.push_back(child->runtimeId());
            edge(container.runtimeId(), child->runtimeId(),
                 ActionClosureEdgeReason::OwnedChild,
                 QStringLiteral("selections"), childPath);
            visitContent(*child, childPath, control);
        }
        const auto visitRows = [&](const auto& rows, const QString& field) {
            for (const auto& row : rows) {
                const ProjectItem* target = byGuid(row.target, container.runtimeId(),
                                                   field, path);
                if (!target)
                    continue;
                std::vector<harkenstudio::ObjectId> targetPath = path;
                targetPath.push_back(target->runtimeId());
                edge(container.runtimeId(), target->runtimeId(),
                     ActionClosureEdgeReason::GridRow, field, targetPath);
                visitContent(*target, targetPath, control);
            }
        };
        if (selection->mode == SelectionMode::Match) {
            const auto grid = selection->matchGrid(platform);
            visitRows(grid.rows, QStringLiteral("selections.conditions"));
            if (grid.axes.size() > 1)
                capability(RuntimeCapabilityFeature::MultiAxisGrid,
                           RuntimeCapabilityState::UnavailableEngineContract,
                           container.runtimeId(), QStringLiteral("selections.matchAxes"));
        }
        if (selection->mode == SelectionMode::Blend) {
            const auto mixing = selection->mixing(platform);
            for (const auto& row : mixing.rows)
                visitRows(row.children, QStringLiteral("selections.curves"));
            if (!mixing.empty())
                capability(RuntimeCapabilityFeature::ContinuousGrid,
                           RuntimeCapabilityState::UnavailableEngineContract,
                           container.runtimeId(), QStringLiteral("selections.curves"));
        }
        if (selection->isQueue())
            capability(RuntimeCapabilityFeature::QueueMemory,
                       RuntimeCapabilityState::UnavailableEngineContract,
                       container.runtimeId(), QStringLiteral("playType"));
        if (selection->mode == SelectionMode::Random
            && selection->behavior.discipline == RandomDiscipline::Shuffle)
            capability(RuntimeCapabilityFeature::ShuffleMemory,
                       RuntimeCapabilityState::UnavailableEngineContract,
                       container.runtimeId(), QStringLiteral("behavior.discipline"));
        const bool transitionsActive = selection->isQueue()
            || (selection->mode == SelectionMode::Match
                && selection->variableResponse() == VariableResponse::Live);
        for (size_t i = 0; transitionsActive && i < selection->transitions.rules.size(); ++i) {
            const auto& rule = selection->transitions.rules[i];
            if (!rule.bridge)
                continue;
            const ProjectItem* bridge = byGuid(*rule.bridge, container.runtimeId(),
                                               QStringLiteral("transitions.bridge"), path);
            if (!bridge)
                continue;
            std::vector<harkenstudio::ObjectId> bridgePath = path;
            bridgePath.push_back(bridge->runtimeId());
            edge(container.runtimeId(), bridge->runtimeId(),
                 ActionClosureEdgeReason::TransitionBridge,
                 QStringLiteral("transitions.rules[%1].bridge").arg(i), bridgePath);
            visitContent(*bridge, bridgePath, control);
            capability(RuntimeCapabilityFeature::Transition,
                       RuntimeCapabilityState::UnavailableEngineContract,
                       container.runtimeId(), QStringLiteral("transitions"));
        }
    }

    void visitContent(const ProjectItem& item, const std::vector<harkenstudio::ObjectId>& path,
                      bool control = false)
    {
        if (!isContentType(item.type)) {
            diagnostic(ActionClosureDiagnosticCode::InvalidReferenceType,
                       QStringLiteral("%1 is not a content object").arg(item.name),
                       plan.rootActionStableId, -1, item.runtimeId(),
                       QStringLiteral("content"), path);
            return;
        }
        if (harkenstudio::IsNull(item.runtimeId()))
            return;
        if (!(control ? controlVisited : objectVisited).insert(item.runtimeId()).second)
            return;
        (control ? plan.controlObjects : plan.objects).push_back(member(item, path));
        addDependencies(item, path);
        switch (item.type) {
        case ProjectItem::Type::Sound:
            addSoundMedia(item, control);
            break;
        case ProjectItem::Type::Container:
            visitContainer(item, path, control);
            break;
        case ProjectItem::Type::Track:
            visitTrack(item, path, control);
            break;
        case ProjectItem::Type::Segment:
            for (const auto& child : item.children) {
                if (!child || child->type != ProjectItem::Type::Track)
                    continue;
                std::vector<harkenstudio::ObjectId> childPath = path;
                childPath.push_back(child->runtimeId());
                edge(item.runtimeId(), child->runtimeId(),
                     ActionClosureEdgeReason::OwnedChild,
                     QStringLiteral("tracks"), childPath);
                visitContent(*child, childPath, control);
            }
            break;
        default:
            break;
        }
    }

    void visitAction(const ProjectItem& action, std::vector<harkenstudio::ObjectId> path)
    {
        if (harkenstudio::IsNull(action.runtimeId()))
            return;
        if (actionActive.count(action.runtimeId())) {
            diagnostic(ActionClosureDiagnosticCode::PostActionCycle,
                       QStringLiteral("PostAction cycle reaches '%1'").arg(action.name),
                       action.runtimeId(), -1, action.runtimeId(),
                       QStringLiteral("steps"), path);
            return;
        }
        if (!actionVisited.insert(action.runtimeId()).second)
            return;
        actionActive.insert(action.runtimeId());
        if (path.empty() || path.back() != action.runtimeId())
            path.push_back(action.runtimeId());
        plan.actions.push_back(member(action, path));
        const auto* component = action.comp<VAction>();
        if (!component) {
            diagnostic(ActionClosureDiagnosticCode::SchemaMismatch,
                       QStringLiteral("Action has no Action component"),
                       action.runtimeId(), -1, action.runtimeId(),
                       QStringLiteral("steps"), path);
            actionActive.erase(action.runtimeId());
            return;
        }
        for (size_t i = 0; i < component->steps.size(); ++i) {
            const auto& step = component->steps[i];
            const bool quantized = (step.type == VActionStep::Type::Play
                                    || step.type == VActionStep::Type::Stop
                                    || step.type == VActionStep::Type::PostAction)
                && step.quantize.timing != QuantizeTiming::Immediate;
            if (quantized && (step.quantize.referenceObjectID != 0
                              || !step.quantize.referenceName.isEmpty())) {
                const QString field = QStringLiteral("steps[%1].quantize.referenceObjectID").arg(i);
                const auto resolved = EditorUtils::resolveActionStepTarget(
                    VActionStep::Type::Play, step.quantize.referenceObjectID,
                    step.quantize.referenceName, targetLookup);
                const ProjectItem* reference = byObject(
                    resolved.state == EditorUtils::ActionTargetState::Resolved
                        ? resolved.objectID : step.quantize.referenceObjectID,
                    action.runtimeId(), static_cast<int>(i), field, path);
                if (reference) {
                    std::vector<harkenstudio::ObjectId> referencePath = path;
                    referencePath.push_back(reference->runtimeId());
                    edge(action.runtimeId(), reference->runtimeId(),
                         ActionClosureEdgeReason::QuantizeReference,
                         field, referencePath);
                    visitContent(*reference, referencePath, step.type == VActionStep::Type::Stop);
                }
            }
            if (quantized)
                capability(RuntimeCapabilityFeature::Quantize,
                           RuntimeCapabilityState::UnavailableEngineContract,
                           action.runtimeId(),
                           QStringLiteral("steps[%1].quantize.timing").arg(i));
            if (quantized && step.quantize.ifSilent != QuantizeIfSilent::Immediate)
                capability(RuntimeCapabilityFeature::IfSilent,
                           RuntimeCapabilityState::UnavailableEngineContract,
                           action.runtimeId(),
                           QStringLiteral("steps[%1].quantize.ifSilent").arg(i));
            if (step.type == VActionStep::Type::SetVariable) {
                capability(RuntimeCapabilityFeature::ActionSetVariable,
                           RuntimeCapabilityState::Ready, action.runtimeId(),
                           QStringLiteral("steps[%1]").arg(i));
            } else if (step.type == VActionStep::Type::Reset) {
                capability(RuntimeCapabilityFeature::ActionReset,
                           RuntimeCapabilityState::Ready, action.runtimeId(),
                           QStringLiteral("steps[%1]").arg(i));
                continue;
            }
            const auto resolution = EditorUtils::resolveActionStepTarget(
                step.type, step.targetObjectID, step.targetName, targetLookup);
            const uint32_t targetObjectId = resolution.state == EditorUtils::ActionTargetState::Resolved
                ? resolution.objectID : step.targetObjectID;
            const ProjectItem* target = byObject(targetObjectId, action.runtimeId(),
                                                 static_cast<int>(i),
                                                 QStringLiteral("steps.targetObjectID"), path);
            if (!target)
                continue;
            std::vector<harkenstudio::ObjectId> targetPath = path;
            targetPath.push_back(target->runtimeId());
            if (step.type == VActionStep::Type::SetVariable) {
                if (target->type != ProjectItem::Type::Variable) {
                    diagnostic(ActionClosureDiagnosticCode::InvalidReferenceType,
                               QStringLiteral("Set Variable target is not a Variable"),
                               action.runtimeId(), static_cast<int>(i),
                               target->runtimeId(), QStringLiteral("steps"),
                               targetPath);
                    continue;
                }
                edge(action.runtimeId(), target->runtimeId(),
                     ActionClosureEdgeReason::ActionStep,
                     QStringLiteral("steps[%1]").arg(i), targetPath);
                continue;
            }
            if (step.type == VActionStep::Type::PostAction) {
                if (target->type != ProjectItem::Type::Action) {
                    diagnostic(ActionClosureDiagnosticCode::InvalidReferenceType,
                               QStringLiteral("PostAction target is not an Action"),
                               action.runtimeId(), static_cast<int>(i),
                               target->runtimeId(), QStringLiteral("steps"), targetPath);
                    continue;
                }
                edge(action.runtimeId(), target->runtimeId(),
                     ActionClosureEdgeReason::PostAction,
                     QStringLiteral("steps[%1]").arg(i), targetPath);
                if (actionActive.count(target->runtimeId())) {
                    diagnostic(ActionClosureDiagnosticCode::PostActionCycle,
                               QStringLiteral("PostAction cycle reaches '%1'").arg(target->name),
                               action.runtimeId(), static_cast<int>(i),
                               target->runtimeId(), QStringLiteral("steps"), targetPath);
                    continue;
                }
                visitAction(*target, targetPath);
                continue;
            }
            if (step.type != VActionStep::Type::Play
                && step.type != VActionStep::Type::Stop)
                continue;
            edge(action.runtimeId(), target->runtimeId(),
                 ActionClosureEdgeReason::ActionStep,
                 QStringLiteral("steps[%1].%2")
                     .arg(i).arg(step.type == VActionStep::Type::Play
                                     ? QStringLiteral("play") : QStringLiteral("stop")),
                 targetPath);
            visitContent(*target, targetPath, step.type == VActionStep::Type::Stop);
        }
        actionActive.erase(action.runtimeId());
    }

    ActionClosurePlan build(harkenstudio::ObjectId actionStableId)
    {
        plan.rootActionStableId = actionStableId;
        if (!variant.platform.isEmpty()) {
            const auto id = ProjectPlatformSemantics::parseId(variant.platform);
            if (!id || !ProjectPlatformSemantics::find(project.platforms, *id)) {
                diagnostic(ActionClosureDiagnosticCode::InvalidBuildPlatform,
                    QStringLiteral("Unknown project platform ID: '%1'").arg(variant.platform),
                    actionStableId, -1, {}, QStringLiteral("platformId"), {});
                return finish();
            }
        }
        const auto it = index.stable.find(actionStableId);
        if (harkenstudio::IsNull(actionStableId) || it == index.stable.end()
            || !it->second || it->second->type != ProjectItem::Type::Action) {
            diagnostic(ActionClosureDiagnosticCode::MissingRootAction,
                       QStringLiteral("Action stable identity %1 does not resolve")
                           .arg(objectIdString(actionStableId)),
                       actionStableId, -1, actionStableId,
                       QStringLiteral("rootActionStableId"), {});
            return finish();
        }
        visitAction(*it->second, {});
        return finish();
    }

    ActionClosurePlan finish()
    {
        const auto memberLess = [](const ActionClosureMember& a,
                                   const ActionClosureMember& b) {
            return a.stableId < b.stableId;
        };
        std::sort(plan.actions.begin(), plan.actions.end(), memberLess);
        std::sort(plan.objects.begin(), plan.objects.end(), memberLess);
        std::sort(plan.controlObjects.begin(), plan.controlObjects.end(), memberLess);
        const auto mediaLess = [](const auto& a, const auto& b) {
            return std::tie(a.soundStableId, a.language, a.sourceEntryId)
                < std::tie(b.soundStableId, b.language, b.sourceEntryId);
        };
        std::sort(plan.media.begin(), plan.media.end(), mediaLess);
        std::sort(plan.controlMedia.begin(), plan.controlMedia.end(), mediaLess);
        std::sort(plan.edges.begin(), plan.edges.end(), [](const auto& a, const auto& b) {
            return std::tie(a.fromStableId, a.toStableId, a.reason, a.field)
                < std::tie(b.fromStableId, b.toStableId, b.reason, b.field);
        });
        std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
            return std::tie(a.code, a.actionStableId, a.stepIndex, a.objectStableId, a.field, a.message)
                < std::tie(b.code, b.actionStableId, b.stepIndex, b.objectStableId, b.field, b.message);
        });
        std::sort(plan.capabilities.begin(), plan.capabilities.end(), [](const auto& a, const auto& b) {
            return std::tie(a.feature, a.objectStableId, a.field)
                < std::tie(b.feature, b.objectStableId, b.field);
        });
        return std::move(plan);
    }
};

}

struct ActionClosureBuildContext::Impl
{
    const ProjectData& project;
    std::shared_ptr<const Index> index;
    std::shared_ptr<const EditorUtils::ActionTargetLookup> targetLookup;

    explicit Impl(const ProjectData& value)
        : project(value), index(std::make_shared<Index>(buildIndex(value))),
          targetLookup(std::make_shared<EditorUtils::ActionTargetLookup>(EditorUtils::makeProjectTargetLookup(value)))
    {
    }
};

ActionClosureBuildContext::ActionClosureBuildContext(const ProjectData& project)
    : impl_(std::make_unique<Impl>(project))
{
}

ActionClosureBuildContext::~ActionClosureBuildContext() = default;

ActionClosurePlan ActionClosureBuildContext::build(harkenstudio::ObjectId actionStableId,
    const BuildVariant& variant, const RuntimeCapabilityContext& context) const
{
    return Builder(impl_->project, variant, context, false,
        impl_->index, impl_->targetLookup).build(actionStableId);
}

ActionClosurePlan ActionClosureBuildContext::contentReferences(harkenstudio::ObjectId actionStableId) const
{
    return Builder(impl_->project, {}, {}, true,
        impl_->index, impl_->targetLookup).build(actionStableId);
}

const ProjectItem* ActionClosureBuildContext::findObject(uint32_t objectId) const
{
    const auto found = impl_->index->object.find(objectId);
    return found == impl_->index->object.end() ? nullptr : found->second;
}

const EditorUtils::ActionTargetLookup& ActionClosureBuildContext::targetLookup() const
{
    return *impl_->targetLookup;
}

QJsonObject ActionClosureMember::toJson() const
{
    return {{QStringLiteral("stableId"), objectIdString(stableId)},
            {QStringLiteral("objectId"), static_cast<qint64>(objectId)},
            {QStringLiteral("guid"), guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("type"), itemTypeName(type)},
            {QStringLiteral("name"), name},
            {QStringLiteral("referencePath"), pathJson(referencePath)}};
}

QJsonObject ActionClosureMedia::toJson() const
{
    return {{QStringLiteral("soundStableId"), objectIdString(soundStableId)},
            {QStringLiteral("sourceEntryId"), sourceEntryId.toString(QUuid::WithoutBraces)},
            {QStringLiteral("authoringSourceId"), static_cast<qint64>(authoringSourceId)},
            {QStringLiteral("platform_id"), platform},
            {QStringLiteral("language"), language},
            {QStringLiteral("artifactId"), artifactId}};
}

QJsonObject ActionClosureEdge::toJson() const
{
    return {{QStringLiteral("fromStableId"), objectIdString(fromStableId)},
            {QStringLiteral("toStableId"), objectIdString(toStableId)},
            {QStringLiteral("reason"), ActionClosureSemantics::edgeReasonName(reason)},
            {QStringLiteral("field"), field},
            {QStringLiteral("referencePath"), pathJson(referencePath)}};
}

QJsonObject ActionClosureDiagnostic::toJson() const
{
    return {{QStringLiteral("code"), ActionClosureSemantics::diagnosticCodeName(code)},
            {QStringLiteral("message"), message},
            {QStringLiteral("actionStableId"), objectIdString(actionStableId)},
            {QStringLiteral("stepIndex"), stepIndex},
            {QStringLiteral("objectStableId"), objectIdString(objectStableId)},
            {QStringLiteral("field"), field},
            {QStringLiteral("referencePath"), pathJson(referencePath)}};
}

QJsonObject ActionClosureCapabilityRequirement::toJson() const
{
    return {{QStringLiteral("feature"), RuntimeCapabilityMatrix::featureName(feature)},
            {QStringLiteral("state"), RuntimeCapabilityMatrix::stateName(state)},
            {QStringLiteral("objectStableId"), objectIdString(objectStableId)},
            {QStringLiteral("field"), field}};
}

bool ActionClosurePlan::ready() const
{
    return diagnostics.empty()
        && std::none_of(capabilities.begin(), capabilities.end(), [](const auto& requirement) {
        return requirement.state == RuntimeCapabilityState::UnavailableEngineContract
            || requirement.state == RuntimeCapabilityState::InvalidAuthoring;
    });
}

QJsonObject ActionClosurePlan::toJson(const ProjectData& project) const
{
    const auto index = buildIndex(project);
    const auto members = [&index](const std::vector<ActionClosureMember>& source) {
        QJsonArray result;
        for (const auto& member : source) {
            auto value = member.toJson();
            const auto found = index.guid.find(member.guid);
            if (found != index.guid.end())
                value.insert(QStringLiteral("objectPath"), found->second->objectPath());
            result.append(value);
        }
        return result;
    };
    const auto values = [](const auto& source) {
        QJsonArray result;
        for (const auto& value : source)
            result.append(value.toJson());
        return result;
    };
    return {{QStringLiteral("variant"), QJsonObject{
                {QStringLiteral("platform_id"), variant.platform},
                {QStringLiteral("language"), variant.language}}},
            {QStringLiteral("rootActionStableId"), objectIdString(rootActionStableId)},
            {QStringLiteral("actions"), members(actions)},
            {QStringLiteral("objects"), members(objects)},
            {QStringLiteral("media"), values(media)},
            {QStringLiteral("controlObjects"), members(controlObjects)},
            {QStringLiteral("controlMedia"), values(controlMedia)},
            {QStringLiteral("edges"), values(edges)},
            {QStringLiteral("diagnostics"), values(diagnostics)},
            {QStringLiteral("capabilities"), values(capabilities)},
            {QStringLiteral("ready"), ready()}};
}

namespace ActionClosureSemantics {

QString edgeReasonName(ActionClosureEdgeReason reason)
{
    switch (reason) {
    case ActionClosureEdgeReason::ActionStep: return QStringLiteral("ActionStep");
    case ActionClosureEdgeReason::PostAction: return QStringLiteral("PostAction");
    case ActionClosureEdgeReason::OwnedChild: return QStringLiteral("OwnedChild");
    case ActionClosureEdgeReason::TrackClip: return QStringLiteral("TrackClip");
    case ActionClosureEdgeReason::GridRow: return QStringLiteral("GridRow");
    case ActionClosureEdgeReason::TransitionBridge: return QStringLiteral("TransitionBridge");
    case ActionClosureEdgeReason::EffectiveOutputBus: return QStringLiteral("EffectiveOutputBus");
    case ActionClosureEdgeReason::EffectiveAttenuation: return QStringLiteral("EffectiveAttenuation");
    case ActionClosureEdgeReason::EffectiveSend: return QStringLiteral("EffectiveSend");
    case ActionClosureEdgeReason::BindingDependency: return QStringLiteral("BindingDependency");
    case ActionClosureEdgeReason::EffectDependency: return QStringLiteral("EffectDependency");
    case ActionClosureEdgeReason::QuantizeReference: return QStringLiteral("QuantizeReference");
    }
    return {};
}

QString diagnosticCodeName(ActionClosureDiagnosticCode code)
{
    switch (code) {
    case ActionClosureDiagnosticCode::MissingRootAction: return QStringLiteral("MissingRootAction");
    case ActionClosureDiagnosticCode::MissingStableIdentity: return QStringLiteral("MissingStableIdentity");
    case ActionClosureDiagnosticCode::DuplicateStableIdentity: return QStringLiteral("DuplicateStableIdentity");
    case ActionClosureDiagnosticCode::DanglingReference: return QStringLiteral("DanglingReference");
    case ActionClosureDiagnosticCode::InvalidReferenceType: return QStringLiteral("InvalidReferenceType");
    case ActionClosureDiagnosticCode::PostActionCycle: return QStringLiteral("PostActionCycle");
    case ActionClosureDiagnosticCode::UnsupportedCapability: return QStringLiteral("UnsupportedCapability");
    case ActionClosureDiagnosticCode::AmbiguousSourceSelection: return QStringLiteral("AmbiguousSourceSelection");
    case ActionClosureDiagnosticCode::SchemaMismatch: return QStringLiteral("SchemaMismatch");
    case ActionClosureDiagnosticCode::LocalizedWithoutLanguages: return QStringLiteral("LocalizedWithoutLanguages");
    case ActionClosureDiagnosticCode::InvalidBuildLanguage: return QStringLiteral("InvalidBuildLanguage");
    case ActionClosureDiagnosticCode::InvalidBuildPlatform: return QStringLiteral("InvalidBuildPlatform");
    case ActionClosureDiagnosticCode::ExcludedDependency: return QStringLiteral("ExcludedDependency");
    case ActionClosureDiagnosticCode::InvalidPackName: return QStringLiteral("InvalidPackName");
    }
    return {};
}

ActionClosurePlan buildForContext(const ProjectData& project, harkenstudio::ObjectId actionStableId,
                                  const BuildVariant& variant,
                                  const RuntimeCapabilityContext& context,
                                  const ActionClosureBuildContext* buildContext)
{
    if (buildContext) return buildContext->build(actionStableId, variant, context);
    return Builder(project, variant, context).build(actionStableId);
}

ActionClosurePlan contentReferences(const ProjectData& project,
                                    harkenstudio::ObjectId actionStableId)
{
    return Builder(project, {}, {}, true).build(actionStableId);
}

}
}
