
#include "../ActionManager.h"
#include "../AttenuationStore.h"
#include "../AuthoringPolicy.h"
#include "../ItemTypeCatalog.h"
#include "../ItemPropertySchema.h"
#include "../ProjectManager.h"
#include "../AssetPipeline/PathUtils.h"
#include "../ProjectLanguageSemantics.h"
#include "../NameValidator.h"
#include "../ProjectPackExporter.h"
#include "../PlayablePropertyAuthoringV1.h"
#include "../FieldGroups.h"
#include "../LanguageContextService.h"
#include "../PlatformContext.h"
#include "../PlatformContextService.h"
#include "../PropertyEditSession.h"
#include "../ActionStepEditing.h"
#include "../ActionStepUtils.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "../UndoCommands.h"
#include "../Components/VAudio.h"
#include "../Components/VMixNode.h"
#include "../Components/VSpatial.h"
#include "../Components/VBus.h"
#include "../Components/VAction.h"
#include "../Components/VSelection.h"
#include "../Components/VSound.h"
#include "../ContainerBehaviorSemantics.h"
#include "../GridSemantics.h"
#include "../SoundSourceEditing.h"
#include "../SoundSourceSemantics.h"
#include "../AssetPipeline/AssetRegistry.h"
#include "../Components/VVariable.h"
#include "../VariableConsumerSemantics.h"
#include "../Components/VTrack.h"
#include "../Components/VPiece.h"
#include "../Timeline/TimelineEditing.h"
#include "../AudioEngine.h"
#include "../../Widgets/ProjectExplorer/ProjectExplorer.h"
#include "../../Widgets/Hierarchy/Hierarchy.h"
#include "ActionSelectors.h"

#include <QDir>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace VNX {

ProjectItem* findItemByPath(ProjectItem* root, const QString& path);
ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);
ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
ProjectItem* findItemByAuthoringHandleInProject(ProjectData* project, uint32_t authoringHandle);
uint32_t itemShortID(const ProjectItem* item);
ProjectItem* resolveItemByPathOrObjectID(ProjectData* project,
                                         const QString& path,
                                         uint32_t objectID);
QString buildItemFullPath(ProjectData* project, ProjectItem* item);
QString itemTypeToString(ProjectItem::Type type);
std::optional<ProjectItem::Type> stringToItemType(const QString& typeStr);
ActionResult applyBusPropertiesArgs(const QJsonObject& args, const QObject* originator);

namespace {

bool jsonValueAsBool(const QJsonValue& valueArg)
{
    if (valueArg.isBool()) return valueArg.toBool();
    if (valueArg.isDouble()) return valueArg.toDouble(0.0) != 0.0;
    const QString s = valueArg.toString().trimmed().toLower();
    return s == QStringLiteral("true") || s == QStringLiteral("1")
        || s == QStringLiteral("yes") || s == QStringLiteral("on");
}

bool claimsBusItemProperty(ProjectItem* item, const QString& propertyName)
{
    static const QSet<QString> kBusProps = {
        QStringLiteral("pan"),
        QStringLiteral("outputBusId"), QStringLiteral("outputBus"), QStringLiteral("output_bus"),
        QStringLiteral("outputBusVolume")};
    return item->comp<VBus>() != nullptr && kBusProps.contains(propertyName);
}

bool claimsAudioItemProperty(ProjectItem* item, const QString& propertyName)
{
    Q_UNUSED(item); Q_UNUSED(propertyName);
    return true;
}

bool claimsPlayableItemProperty(ProjectItem* item, const QString& propertyName)
{
    if (!item || !PlayablePropertyAuthoringV1::isDefinitionType(item->type)
        || PlayablePropertyAuthoringV1::fieldByName(propertyName) == nullptr)
        return false;
    if (item->type == ProjectItem::Type::Bus
        && propertyName == QStringLiteral("outputBusVolume"))
        return false;
    return true;
}

bool claimsFieldGroupItemProperty(const ProjectItem& item, const QString& propertyName)
{
    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type))
        return false;
    if (const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName))
        return FieldGroupForProperty(
            static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field->id)).has_value();
    return propertyName == QStringLiteral("outputBusId")
        || propertyName == QStringLiteral("outputBus")
        || propertyName == QStringLiteral("output_bus");
}

enum class PlayableLandingKind { Shared, Overlay };

struct PlayableLandingPlan
{
    PlayableLandingKind kind = PlayableLandingKind::Shared;
    QVector<ProjectPlatformId> selected;
    QString error;
};

PlayableLandingPlan parsePlayableLandingPlan(const QJsonObject& args,
                                             const QStringList& targetPlatforms)
{
    PlayableLandingPlan plan;
    if (args.contains(QStringLiteral("platforms"))) {
        plan.error = QStringLiteral("Use platform_ids for project platform references");
        return plan;
    }
    if (!args.contains(QStringLiteral("platform_ids"))) {
        plan.selected = occupiedTargetPlatforms(targetPlatforms);
        return plan;
    }
    const QJsonValue raw = args.value(QStringLiteral("platform_ids"));
    if (!raw.isArray()) {
        plan.error = QStringLiteral("platform_ids must be an array of platform IDs");
        return plan;
    }
    const QJsonArray names = raw.toArray();
    if (names.isEmpty()) {
        plan.error = QStringLiteral("platform_ids must not be empty");
        return plan;
    }
    QSet<ProjectPlatformId> seen;
    for (const QJsonValue& entry : names) {
        if (!entry.isString()) {
            plan.error = QStringLiteral("platform_ids entries must be strings");
            return plan;
        }
        const QString name = entry.toString();
        const ProjectPlatformId platform = QUuid::fromString(name);
        if (platform == ProjectPlatformId{}
            || ProjectPlatformSemantics::idString(platform) != name) {
            plan.error = QStringLiteral("platform_ids contains an invalid platform ID");
            return plan;
        }
        if (seen.contains(platform)) {
            plan.error = QStringLiteral("platform_ids contains a duplicate platform ID");
            return plan;
        }
        seen.insert(platform);
        plan.selected.append(platform);
    }
    if (plan.selected.isEmpty()) {
        plan.error = QStringLiteral("platform_ids must not be empty");
        return plan;
    }
    if (!selectedIsSubsetOfTargetPlatforms(plan.selected, targetPlatforms)) {
        plan.error = QStringLiteral(
            "platform_ids contains a platform outside the project platform definitions");
        return plan;
    }
    plan.kind = PlayableLandingKind::Overlay;
    return plan;
}

std::optional<PlayablePropertyStateV1> readLandingState(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    PlatformScope scope)
{
    if (!scope)
        return PlayablePropertyAuthoringV1::readProperty(
            owner, locator, propertyId, std::nullopt);
    return PlayablePropertyAuthoringV1::readPlatformOverlay(
        owner, locator, propertyId, *scope);
}

QVector<PlatformScope> playableLandingScopes(const PlayableLandingPlan& plan,
                                            const QString& operation)
{
    QVector<PlatformScope> scopes;
    if (operation == QLatin1String("clear_platform_override")) {
        for (ProjectPlatformId platform : plan.selected)
            scopes.append(platform);
        return scopes;
    }
    if (plan.kind == PlayableLandingKind::Overlay) {
        for (ProjectPlatformId platform : plan.selected)
            scopes.append(platform);
        return scopes;
    }
    scopes.append(std::nullopt);
    if (plan.kind == PlayableLandingKind::Shared) {
        for (ProjectPlatformId platform : plan.selected)
            scopes.append(platform);
    }
    return scopes;
}

bool applyPlayableLanding(ProjectItem& item,
                          const PlayablePropertyLocatorV1& locator,
                          uint8_t propertyId,
                          const QString& operation,
                          const QJsonValue& value,
                          PlatformScope scope,
                          PlayableLandingKind kind)
{
    if (operation == QLatin1String("set_override")) {
        if (!scope)
            return PlayablePropertyAuthoringV1::setOverride(
                item, locator, propertyId, value, std::nullopt);
        if (kind == PlayableLandingKind::Shared)
            return PlayablePropertyAuthoringV1::clearOverride(
                item, locator, propertyId, scope);
        return PlayablePropertyAuthoringV1::setOverride(
            item, locator, propertyId, value, scope);
    }
    if (operation == QLatin1String("clear_override"))
        return PlayablePropertyAuthoringV1::clearOverride(
            item, locator, propertyId, scope);
    if (operation == QLatin1String("clear_platform_override")) {
        if (!scope)
            return false;
        return PlayablePropertyAuthoringV1::clearOverride(
            item, locator, propertyId, scope);
    }
    return false;
}

std::optional<PlayablePropertyLocatorV1> playableLocatorFromArgs(
    ProjectItem& item, const QJsonObject& args)
{
    const QString kind = args.value(QStringLiteral("occurrence_kind")).toString();
    if (kind.isEmpty()) return PlayablePropertyAuthoringV1::definitionLocator(item);
    const QUuid occurrenceGuid = QUuid::fromString(
        args.value(QStringLiteral("occurrence_guid")).toString());
    const QJsonValue ordinalValue = args.value(QStringLiteral("occurrence_ordinal"));
    const qint64 ordinal = ordinalValue.toInteger(-1);
    if (occurrenceGuid.isNull() || !ordinalValue.isDouble()
        || ordinal < 0 || ordinal > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    std::optional<PlayablePropertyLocatorV1> locator;
    if (kind == QLatin1String("track_clip"))
        locator = PlayablePropertyAuthoringV1::trackClipLocator(item, occurrenceGuid);
    else if (kind == QLatin1String("container_child"))
        locator = PlayablePropertyAuthoringV1::containerChildLocator(item, occurrenceGuid);
    if (!locator || locator->stableOccurrenceOrdinal != static_cast<uint32_t>(ordinal))
        return std::nullopt;
    const QString targetText = args.value(QStringLiteral("occurrence_target_guid")).toString();
    if (targetText.isEmpty()) return std::nullopt;
    const QUuid targetGuid = QUuid::fromString(targetText);
    if (targetGuid.isNull() || locator->targetGuid != targetGuid)
        return std::nullopt;
    return locator;
}

bool itemMatchesReferenceRole(
    const ProjectItem& item,
    harkenplayableproperties::PlayablePropertyReferenceRoleV1 role)
{
    using Role = harkenplayableproperties::PlayablePropertyReferenceRoleV1;
    return (role == Role::MainOutputBus && item.type == ProjectItem::Type::Bus)
        || (role == Role::Attenuation
            && item.type == ProjectItem::Type::Attenuation);
}

bool validatePlayableReference(
    ProjectManager& manager,
    const PlayablePropertyAuthoringV1::Field& field,
    const QJsonValue& value)
{
    using Role = harkenplayableproperties::PlayablePropertyReferenceRoleV1;
    if (field.referenceRole == Role::None) return true;
    if (!value.isString()) return false;
    harkenstudio::ObjectId stableId{};
    if (!parseObjectIdJson(value, &stableId)) return false;
    if (harkenstudio::IsNull(stableId)) return true;
    ProjectItem* target = manager.findItemByRuntimeStableId(stableId);
    return target && itemMatchesReferenceRole(*target, field.referenceRole);
}

bool validatePlayableSends(ProjectManager& manager,
                           const ProjectItem& owner,
                           const QJsonValue& value)
{
    if (!value.isArray()) return false;
    QSet<uint32_t> targets;
    for (const QJsonValue& rowValue : value.toArray()) {
        if (!rowValue.isObject()) return false;
        const QJsonObject row = rowValue.toObject();
        const QJsonValue targetValue = row.value(QStringLiteral("targetBusID"));
        const QJsonValue levelValue = row.value(QStringLiteral("sendLevel"));
        if (!targetValue.isDouble() || !levelValue.isDouble()) return false;
        const double targetNumber = targetValue.toDouble(-1.0);
        const double level = levelValue.toDouble();
        if (!std::isfinite(targetNumber) || std::trunc(targetNumber) != targetNumber
            || targetNumber <= 0.0
            || targetNumber > static_cast<double>(std::numeric_limits<uint32_t>::max())
            || !std::isfinite(level) || level < -96.0 || level > 24.0)
            return false;
        const uint32_t target = static_cast<uint32_t>(targetNumber);
        if (targets.contains(target)
            || !manager.sendTargetStructurallySelectable(owner.objectID, target))
            return false;
        targets.insert(target);
    }
    return true;
}

bool claimsStepsItemProperty(ProjectItem* item, const QString& propertyName)
{
    return propertyName == QStringLiteral("steps")
        && item->type == ProjectItem::Type::Action;
}

TimelineValidation validateAuthoredReference(ProjectManager* manager,
                                             const ProjectItem* item)
{
    if (!item) return TimelineValidation::success();
    return AuthoringPolicy::validateNewSubtree(*item, nullptr,
        [manager](const QUuid& guid) { return manager->findItemByGuid(guid); },
        [manager](uint32_t objectID) { return manager->findItemByObjectID(objectID); });
}


}

void applyStepsList(uint32_t objectID, const std::vector<VActionStep>& target)
{
    auto& pm = ProjectManager::instance();
    ProjectData* project = pm.currentProject();
    if (!project) return;
    ProjectItem* item = findItemByObjectIDInProject(project, objectID);
    if (!item || item->type != ProjectItem::Type::Action) return;

    item->compOrCreate<VAction>()->steps = target;
    item->markDirty();
    ProjectManager::resyncItemToRuntime(item);
    emit pm.itemPropertyChanged(item, QStringLiteral("steps"));
}

ActionResult handleStepsItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                     const QString& propertyName, const QJsonObject& args,
                                     const QObject* originator)
{
    Q_UNUSED(originator);

    const QJsonValue value = args.value(QStringLiteral("value"));
    if (!value.isArray())
        return {false, {}, "INVALID_ARGS",
                QStringLiteral("Property 'steps' value must be a JSON array")};

    auto* action = item->compOrCreate<VAction>();
    const std::vector<VActionStep> oldSteps = action->steps;
    QString parseError;
    auto parsedSteps = ActionStepEditing::checkedStepsFromJson(
        value.toArray(), &parseError);
    if (!parsedSteps)
        return {false, {}, "INVALID_ARGS", parseError};
    std::vector<VActionStep> newSteps = std::move(*parsedSteps);
    const QJsonArray oldJson = ActionStepEditing::stepsToJson(oldSteps);
    const QJsonArray requestedJson = ActionStepEditing::stepsToJson(newSteps);
    std::vector<bool> retainedSteps(oldSteps.size(), false);
    ProjectItem authoredAction(item->name, ProjectItem::Type::Action);
    authoredAction.objectID = item->objectID;
    auto* authoredSteps = authoredAction.compOrCreate<VAction>();

    if (ProjectData* project = projectManager ? projectManager->currentProject() : nullptr)
    {
        const VNX::EditorUtils::ActionTargetLookup lookup =
            VNX::EditorUtils::makeProjectTargetLookup(*project);
        for (size_t index = 0; index < newSteps.size(); ++index)
        {
            VActionStep& step = newSteps[index];
            size_t retainedIndex = 0;
            for (; retainedIndex < oldSteps.size(); ++retainedIndex)
                if (!retainedSteps[retainedIndex]
                    && oldJson.at(static_cast<qsizetype>(retainedIndex))
                        == requestedJson.at(static_cast<qsizetype>(index)))
                    break;
            if (retainedIndex < oldSteps.size()) {
                retainedSteps[retainedIndex] = true;
                step = oldSteps[retainedIndex];
                continue;
            }

            const auto resolution = VNX::EditorUtils::resolveActionStepTarget(
                step.type, step.targetObjectID, step.targetName, lookup,
                item->objectID);
            const VNX::EditorUtils::ActionTargetRecord* resolvedTarget =
                resolution.state == VNX::EditorUtils::ActionTargetState::Resolved
                    ? lookup.findByObjectID(resolution.objectID) : nullptr;
            if (!resolvedTarget && step.targetObjectID != 0) {
                const VNX::EditorUtils::ActionTargetRecord* target =
                    lookup.findByObjectID(step.targetObjectID);
                if (target && !VNX::EditorUtils::canActionStepReferenceObject(
                                  step.type, target->type, target->objectID,
                                  item->objectID)) {
                    return {false, {}, "INVALID_TARGET",
                            QStringLiteral("Action step %1 target '%2' is not valid for its step type")
                                .arg(index).arg(target->name)};
                }
            }
            if (resolvedTarget) {
                step.targetObjectID = resolvedTarget->objectID;
                step.targetName = resolvedTarget->name;
                if (step.type == VActionStep::Type::SetVariable) {
                    ProjectItem* variable = projectManager->findItemByObjectID(
                        resolvedTarget->objectID);
                    const VVariable* declaration = variable
                        ? variable->comp<VVariable>() : nullptr;
                    if (!declaration)
                        return {false, {}, "INVALID_TARGET",
                                QStringLiteral("Action step %1 Set Variable target has no Variable declaration")
                                    .arg(index)};
                    if (!std::isfinite(step.value) ||
                        step.value < declaration->rangeMin ||
                        step.value > declaration->rangeMax ||
                        (declaration->kind == VariableKind::Step &&
                         (step.value < -16777216.0f ||
                          step.value > 16777216.0f ||
                          step.value != std::floor(step.value)))) {
                        return {false, {}, "INVALID_VALUE",
                                QStringLiteral("Action step %1 Set Variable is not a legal value for its Variable declaration")
                                    .arg(index)};
                    }
                }
            }

            const auto* catalogEntry =
                VNX::EditorUtils::actionStepCatalogEntry(step.type);
            const bool supportsQuantize =
                catalogEntry && catalogEntry->supportsQuantize;
            const VNX::EditorUtils::ActionTargetRecord* resolvedReference = nullptr;
            if (supportsQuantize && step.quantize.referenceObjectID != 0) {
                const VNX::EditorUtils::ActionTargetRecord* reference =
                    lookup.findByObjectID(step.quantize.referenceObjectID);
                if (reference && !ItemTypeCatalog::isContentObject(reference->type)) {
                    return {false, {}, "INVALID_TARGET",
                            QStringLiteral("Action step %1 quantize reference '%2' is not a content object")
                                .arg(index).arg(reference->name)};
                }
                resolvedReference = reference;
            } else if (supportsQuantize && !step.quantize.referenceName.isEmpty()) {
                const auto reference = VNX::EditorUtils::resolveActionStepTarget(
                    VActionStep::Type::Play, 0, step.quantize.referenceName, lookup);
                if (reference.state == VNX::EditorUtils::ActionTargetState::Resolved)
                    resolvedReference = lookup.findByObjectID(reference.objectID);
            }
            if (resolvedReference) {
                step.quantize.referenceObjectID = resolvedReference->objectID;
                step.quantize.referenceName = resolvedReference->name;
            }
            authoredSteps->steps.push_back(step);
        }
    }

    const auto authoring = validateAuthoredReference(projectManager, &authoredAction);
    if (!authoring)
        return {false, {}, "AUTHORING_RESTRICTED", authoring.message};
    const QJsonArray newJson = ActionStepEditing::stepsToJson(newSteps);

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", propertyName},
        {"value", newJson}
    };

    if (oldJson == newJson)
        return r;

    applyStepsList(item->objectID, newSteps);

    const uint32_t objID = item->objectID;
    r.undoFn = [objID, oldSteps]() { applyStepsList(objID, oldSteps); };
    r.redoFn = [objID, newSteps]() { applyStepsList(objID, newSteps); };
    r.undoText = QObject::tr("Edit Steps");
    return r;
}

ActionResult handleBusItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                   const QString& propertyName, const QJsonObject& args,
                                   const QObject* originator)
{
    Q_UNUSED(projectManager);
    if (propertyName == QStringLiteral("outputBusId")
        || propertyName == QStringLiteral("outputBus")
        || propertyName == QStringLiteral("output_bus")) {
        return {false, {}, "BUS_MAIN_OUTPUT_READONLY",
                QString("Main Output of bus '%1' derives from its parent bus; use move_item instead")
                    .arg(item->name)};
    }
    QJsonObject busArgs;
    busArgs.insert(QStringLiteral("bus_id"), static_cast<qint64>(item->objectID));
    QJsonValue value = args.value(QStringLiteral("value"));
    if (propertyName == QStringLiteral("outputBusVolume")
        && (value.isUndefined() || value.isNull()))
        value = 0.0;
    busArgs.insert(propertyName, value);
    return applyBusPropertiesArgs(busArgs, originator);
}

ActionResult handlePlayableItemProperty(ProjectManager* projectManager,
                                        ProjectItem* item,
                                        const QString& propertyName,
                                        const QJsonObject& args,
                                        const QObject* originator)
{
    const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
    if (!projectManager || !item || !field)
        return {false, {}, "UNKNOWN_PROPERTY", "Playable property is unavailable"};
    if (!PlayablePropertyAuthoringV1::isEditorExposed(field->id))
        return {false, {}, "UNKNOWN_PROPERTY", "Playable property is unavailable"};
    const auto locator = playableLocatorFromArgs(*item, args);
    if (!locator)
        return {false, {}, "INVALID_ARGS", "Playable property occurrence identity is invalid"};
    if (locator->kind != PlayablePropertyTargetKindV1::Definition)
        return {false, {}, "UNKNOWN_PROPERTY", "Playback properties belong to the child object"};
    const QString operation = args.value(QStringLiteral("override_operation"))
        .toString(QStringLiteral("set_override"));
    if (operation != QLatin1String("set_override")
        && operation != QLatin1String("clear_override")
        && operation != QLatin1String("clear_platform_override"))
        return {false, {}, "INVALID_ARGS", "Unknown playable property operation"};
    const PlayableLandingPlan plan = parsePlayableLandingPlan(
        args, projectManager->getTargetPlatforms());
    if (!plan.error.isEmpty())
        return {false, {}, "INVALID_ARGS", plan.error};
    const bool sharedComponent = PlayablePropertyAuthoringV1::isComponentStored(field->id);
    if (sharedComponent && args.contains(QStringLiteral("platform_ids")))
        return {false, {}, "INVALID_ARGS", "Spatialization Mix is shared across platforms"};
    if (operation == QLatin1String("clear_platform_override")
        && !args.contains(QStringLiteral("platform_ids")))
        return {false, {}, "INVALID_ARGS",
                "clear_platform_override requires platform_ids"};
    if (operation == QLatin1String("clear_override")
        || operation == QLatin1String("clear_platform_override"))
        PropertyEditSession::cancelActiveFor(
            item, *locator, field->id);
    QJsonValue value;
    if (operation == QLatin1String("set_override")) {
        if (!args.contains(QStringLiteral("value")))
            return {false, {}, "INVALID_ARGS", "Set Override requires a value"};
        value = args.value(QStringLiteral("value"));
        if (!validatePlayableReference(*projectManager, *field, value))
            return {false, {}, "INVALID_ARGS", "Playable property reference is invalid"};
        if (field->wire == harkenplayableproperties::PlayablePropertyWireV1::RowSpan
            && !validatePlayableSends(*projectManager, *item, value))
            return {false, {}, "INVALID_ARGS", "Playable sends are invalid"};
    }

    const QVector<PlatformScope> scopes = sharedComponent
        ? QVector<PlatformScope>{std::nullopt} : playableLandingScopes(plan, operation);
    if (scopes.isEmpty())
        return {false, {}, "INVALID_ARGS", "Playable property landing is empty"};

    struct ScopeSnapshot {
        PlatformScope scope;
        PlayablePropertyStateV1 oldState;
        PlayablePropertyStateV1 newState;
    };
    std::vector<ScopeSnapshot> snapshots;
    snapshots.reserve(static_cast<size_t>(scopes.size()));
    for (const PlatformScope& scope : scopes) {
        const auto oldState = readLandingState(
            *item, *locator, field->id, scope);
        if (!oldState)
            return {false, {}, "INVALID_ARGS", "Playable property target is unavailable"};
        snapshots.push_back({scope, *oldState, {}});
    }

    auto restoreSnapshots = [item, locator, propertyId = field->id](
                                const std::vector<ScopeSnapshot>& snaps) {
        for (auto it = snaps.rbegin(); it != snaps.rend(); ++it)
            PlayablePropertyAuthoringV1::restoreProperty(
                *item, *locator, propertyId, it->oldState, it->scope);
    };

    for (ScopeSnapshot& snap : snapshots) {
        if (!applyPlayableLanding(*item, *locator, field->id, operation, value,
                                  snap.scope, plan.kind)) {
            restoreSnapshots(snapshots);
            return {false, {}, "INVALID_ARGS",
                    "Playable property value has invalid wire type"};
        }
        const auto newState = readLandingState(
            *item, *locator, field->id, snap.scope);
        if (!newState) {
            restoreSnapshots(snapshots);
            return {false, {}, "SET_PROPERTY_FAILED",
                    "Playable property state is unavailable"};
        }
        snap.newState = *newState;
    }

    bool changed = false;
    for (const ScopeSnapshot& snap : snapshots) {
        if (snap.oldState != snap.newState) {
            changed = true;
            break;
        }
    }
    const auto& reportState = snapshots.front().newState;
    ActionResult result;
    result.success = true;
    result.data = QJsonObject{
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
        {QStringLiteral("property"), QString::fromLatin1(field->fieldName)},
        {QStringLiteral("override_operation"), operation},
        {QStringLiteral("value"), reportState.value},
        {QStringLiteral("present"), reportState.present}};
    if (!changed)
        return result;

    auto publish = [fieldName = QString::fromLatin1(field->fieldName), originator](
                       ProjectItem* target) {
        target->markDirty();
        if (target->type == ProjectItem::Type::Bus) {
            auto& pm = ProjectManager::instance();
            if (fieldName == QStringLiteral("sends"))
                pm.applyBusRuntimeState(target);
            else
                pm.pushBusPropertyLive(target, fieldName);
            emit pm.busChanged(target->objectID, fieldName, originator);
        } else {
            ProjectManager::resyncItemToRuntime(target);
            emit ProjectManager::instance().itemPropertyChanged(
                target, fieldName, originator);
        }
    };
    publish(item);

    const QUuid ownerGuid = item->guid;
    const uint8_t propertyId = field->id;
    auto restoreAll = [ownerGuid, locator = *locator, propertyId](
                          const std::vector<ScopeSnapshot>& snaps, bool toOld) {
        ProjectItem* owner = ProjectManager::instance().findItemByGuid(ownerGuid);
        if (!owner)
            return;
        for (auto it = snaps.rbegin(); it != snaps.rend(); ++it) {
            const PlayablePropertyStateV1& state =
                toOld ? it->oldState : it->newState;
            if (!PlayablePropertyAuthoringV1::restoreProperty(
                    *owner, locator, propertyId, state, it->scope))
                return;
        }
        owner->markDirty();
        const auto* restoredField = PlayablePropertyAuthoringV1::fieldById(propertyId);
        const QString fieldName = restoredField
            ? QString::fromLatin1(restoredField->fieldName) : QString();
        if (owner->type == ProjectItem::Type::Bus) {
            auto& pm = ProjectManager::instance();
            if (fieldName == QStringLiteral("sends"))
                pm.applyBusRuntimeState(owner);
            else
                pm.pushBusPropertyLive(owner, fieldName);
            emit pm.busChanged(owner->objectID, fieldName);
        } else {
            ProjectManager::resyncItemToRuntime(owner);
            if (restoredField)
                emit ProjectManager::instance().itemPropertyChanged(owner, fieldName);
        }
    };
    result.undoFn = [restoreAll, snapshots]() { restoreAll(snapshots, true); };
    result.redoFn = [restoreAll, snapshots]() { restoreAll(snapshots, false); };
    result.undoText = operation == QLatin1String("set_override")
        ? QObject::tr("Set Property Override")
        : QObject::tr("Clear Property Override");
    return result;
}

ActionResult handleAudioItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                     const QString& propertyName, const QJsonObject& args,
                                     const QObject* originator)
{
    const QJsonValue valueArg = args["value"];
    const double value = valueArg.toDouble(0.0);

    auto* audio = item->comp<VAudio>();
    auto* mix = item->comp<VMixNode>();
    auto* spatial = item->comp<VSpatial>();
    static const QSet<QString> kMixBackedProps = {
        QStringLiteral("volume"), QStringLiteral("pitch"),
        QStringLiteral("makeUpGain"), QStringLiteral("makeup_gain"),
        QStringLiteral("outputBusId"), QStringLiteral("outputBus"),
        QStringLiteral("output_bus"), QStringLiteral("outputBusVolume")};
    static const QSet<QString> kSpatialBackedProps = {
        QStringLiteral("spatializationMix"),
        QStringLiteral("attenuationDistanceScaling"),
        QStringLiteral("is3DEnabled")};
    const bool needsMix = kMixBackedProps.contains(propertyName);
    const bool needsSpatial = kSpatialBackedProps.contains(propertyName);
    if ((needsMix && !mix) || (needsSpatial && !spatial)
        || (!needsMix && !needsSpatial && !audio))
        return {false, {}, "UNKNOWN_PROPERTY",
                QString("Property '%1' not applicable to '%2'")
                    .arg(propertyName, item->name)};

    QVariant undoOldValue;
    QVariant undoNewValue;
    bool undoTracked = false;
    bool applied = false;

    if (propertyName == QStringLiteral("loopCount")) {
        const QJsonValue loopValue = args["value"];
        const double raw = loopValue.toDouble(-1.0);
        if (!loopValue.isDouble() || !std::isfinite(raw) || raw != std::trunc(raw)
            || raw < 0.0 || raw > std::numeric_limits<uint32_t>::max())
            return {false, {}, "INVALID_ARGS", "loopCount must be an unsigned 32-bit integer"};
        undoOldValue = QVariant::fromValue(static_cast<qulonglong>(audio->loopCount));
        undoNewValue = QVariant::fromValue(static_cast<qulonglong>(raw));
        applied = projectManager->writeAudioPropertySilent(item, propertyName, undoNewValue);
        undoTracked = true;
    } else if (propertyName == "volume") {
        undoOldValue = mix->volume; undoNewValue = static_cast<float>(value);
        applied = projectManager->writeAudioPropertySilent(item, propertyName, static_cast<float>(value));
        undoTracked = true;
    } else if (propertyName == "pitch") {
        undoOldValue = mix->pitch; undoNewValue = static_cast<float>(value);
        applied = projectManager->writeAudioPropertySilent(item, propertyName, static_cast<float>(value));
        undoTracked = true;
    } else if (propertyName == "makeUpGain" || propertyName == "makeup_gain") {
        undoOldValue = mix->makeUpGain; undoNewValue = static_cast<float>(value);
        applied = projectManager->writeAudioPropertySilent(item, propertyName, static_cast<float>(value));
        undoTracked = true;
    } else if (propertyName == "streamingMode" || propertyName == "streaming_mode") {
        int mode = qBound(0, args["value"].toInt(static_cast<int>(value)), 1);
        undoOldValue = audio->streamingMode; undoNewValue = mode;
        applied = projectManager->writeAudioPropertySilent(item, propertyName, mode);
        undoTracked = true;
    } else if (propertyName == "spatializationMix") {
        undoOldValue = spatial->spatializationMix; undoNewValue = static_cast<float>(value);
        applied = projectManager->writeAudioPropertySilent(item, propertyName, static_cast<float>(value));
        undoTracked = true;
    } else if (propertyName == "attenuationDistanceScaling") {
        float ads = qBound(0.01f, static_cast<float>(value), 100.0f);
        undoOldValue = spatial->attenuationDistanceScaling; undoNewValue = ads;
        applied = projectManager->writeAudioPropertySilent(item, propertyName, ads);
        undoTracked = true;
    } else if (propertyName == "is3DEnabled") {
        bool on = jsonValueAsBool(valueArg);
        undoOldValue = spatial->is3DEnabled; undoNewValue = on;
        applied = projectManager->writeAudioPropertySilent(item, propertyName, on);
        undoTracked = true;
    } else if (propertyName == "outputBusId" || propertyName == "outputBus" || propertyName == "output_bus") {
        uint32_t newBusId = static_cast<uint32_t>(value);
        if (newBusId != 0 && !projectManager->outputBusSelectable(item->objectID, newBusId))
            return {false, {}, "OUTPUT_BUS_REJECTED",
                    QString("Output bus %1 -> %2 rejected: unknown bus or send conflict")
                        .arg(item->name).arg(newBusId)};
        undoOldValue = mix->outputBusId; undoNewValue = newBusId;
        applied = projectManager->writeAudioPropertySilent(item, propertyName, newBusId);
        undoTracked = true;
    } else if (propertyName == "outputBusVolume") {
        undoOldValue = mix->outputBusVolume; undoNewValue = static_cast<float>(value);
        applied = projectManager->writeAudioPropertySilent(item, propertyName, static_cast<float>(value));
        undoTracked = true;
    } else {
        return {false, {}, "UNKNOWN_PROPERTY",
                QString("Unknown property '%1'").arg(propertyName)};
    }

    if (!applied)
        return {false, {}, "SET_PROPERTY_FAILED",
                QString("Failed to set '%1' on '%2'").arg(propertyName, item->name)};

    ProjectManager::resyncItemToRuntime(item);

    projectManager->pushAudioPropertyLive(item, propertyName);

    item->markDirty();
    emit projectManager->itemPropertyChanged(item, propertyName, originator);

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", propertyName},
        {"value", args["value"]}
    };

    if (undoTracked) {
        auto applyFn = [propertyName, projectManager](ProjectItem* pi, const QVariant& val) {
            projectManager->writeAudioPropertySilent(pi, propertyName, val);
            ProjectManager::resyncItemToRuntime(pi);
            pi->markDirty();
            emit ProjectManager::instance().itemPropertyChanged(pi, propertyName);
        };
        uint32_t itemObjID = item->objectID;
        r.undoFn = [applyFn, itemObjID, undoOldValue]() {
            auto* proj = ProjectManager::instance().currentProject();
            if (!proj) return;
            auto* pi = findItemByObjectIDInProject(proj, itemObjID);
            if (!pi) return;
            applyFn(pi, undoOldValue);
        };
        r.redoFn = [applyFn, itemObjID, undoNewValue]() {
            auto* proj = ProjectManager::instance().currentProject();
            if (!proj) return;
            auto* pi = findItemByObjectIDInProject(proj, itemObjID);
            if (!pi) return;
            applyFn(pi, undoNewValue);
        };
        r.undoText = QString("Set %1").arg(propertyName);
    }

    return r;
}

bool claimsSoundItemProperty(ProjectItem* item, const QString& propertyName)
{
    return item->type == ProjectItem::Type::Sound
        && (propertyName == QStringLiteral("sources")
            || propertyName == QStringLiteral("scheduleOffsetSec")
            || propertyName == QStringLiteral("distinguishLanguage"));
}

struct SoundAuthoringState
{
    SoundSources sources;
    double scheduleOffsetSec = 0.0;
    bool distinguishLanguage = false;
};

SoundAuthoringState captureSoundAuthoringState(const VSound& sound)
{
    return {sound.sources, sound.scheduleOffsetSec, sound.distinguishLanguage};
}

void applySoundAuthoringState(uint32_t objectID, const SoundAuthoringState& state,
                              const QString& propertyName,
                              const QObject* originator = nullptr)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* item = manager.findItemByObjectID(objectID);
    auto* sound = item ? item->comp<VSound>() : nullptr;
    if (!sound) return;
    sound->sources = state.sources;
    sound->scheduleOffsetSec = state.scheduleOffsetSec;
    sound->distinguishLanguage = state.distinguishLanguage;
    SoundSourceEditing::syncActiveProjection(
        *item, PlatformContextService::instance().context().primary,
        SoundSourceEditing::previewLanguage(
            *item, LanguageContextService::instance().language()));
    item->markDirty();
    emit manager.itemPropertyChanged(item, propertyName, originator);
    ProjectManager::resyncItemToRuntime(item);
}

ActionResult handleSoundItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                     const QString& propertyName,
                                     const QJsonObject& args,
                                     const QObject* originator)
{
    auto* sound = item->comp<VSound>();
    if (!sound)
        return {false, {}, "UNKNOWN_PROPERTY", "Sound component is unavailable"};
    const SoundAuthoringState oldState = captureSoundAuthoringState(*sound);
    SoundAuthoringState newState = oldState;
    QJsonValue canonicalValue;
    if (propertyName == QStringLiteral("scheduleOffsetSec")) {
        const QJsonValue value = args.value(QStringLiteral("value"));
        if (!value.isDouble() || !std::isfinite(value.toDouble())
            || value.toDouble() < 0.0)
            return {false, {}, "INVALID_ARGS",
                    "scheduleOffsetSec must be a finite non-negative number"};
        newState.scheduleOffsetSec = value.toDouble();
        canonicalValue = newState.scheduleOffsetSec;
    } else if (propertyName == QStringLiteral("distinguishLanguage")) {
        const QJsonValue value = args.value(QStringLiteral("value"));
        if (!value.isBool())
            return {false, {}, "INVALID_ARGS",
                    "distinguishLanguage must be a boolean"};
        newState.distinguishLanguage = value.toBool();
        canonicalValue = newState.distinguishLanguage;
    } else {
        const QJsonValue value = args.value(QStringLiteral("value"));
        if (!value.isObject())
            return {false, {}, "INVALID_ARGS", "sources value must be an object"};
        QString error;
        const auto parsed = SoundSourceSemantics::fromJson(value.toObject(), &error);
        if (!parsed) return {false, {}, "INVALID_ARGS", error};
        for (const auto* set : SoundSourceSemantics::sourceSets(*parsed)) {
            for (const auto& source : set->entries) {
                if (!AssetRegistry::instance().contains(source.sourceId))
                    return {false, {}, "INVALID_ASSET",
                            QStringLiteral("Source asset %1 does not exist").arg(source.sourceId)};
            }
            if (!ProjectPlatformSemantics::validateIds(
                    set->platforms.keys(), projectManager->currentProject()->platforms, &error))
                return {false, {}, "INVALID_ARGS", error};
        }
        const QStringList languages = ProjectLanguageSemantics::buildLanguages(
            *projectManager->currentProject());
        for (auto it = parsed->localized.cbegin(); it != parsed->localized.cend(); ++it) {
            if (!languages.contains(it.key())
                && (!oldState.sources.localized.contains(it.key())
                    || oldState.sources.localized.value(it.key()) != it.value()))
                return {false, {}, "INVALID_ARGS",
                        QStringLiteral("source language is not a project language")};
        }
        newState.sources = *parsed;
        const auto resolveAsset = [](uint32_t sourceId) {
            return AssetRegistry::instance().getAsset(sourceId);
        };
        std::vector<SoundSourceDef> previous;
        for (const auto* set : SoundSourceSemantics::sourceSets(oldState.sources))
            previous.insert(previous.end(), set->entries.begin(), set->entries.end());
        const auto prepareSet = [&](SoundSourceSet& set, const SoundSourceSet& before) {
            if (set.entries == before.entries) return;
            SoundSourceSemantics::settleLoopProvenance(set.entries, resolveAsset);
            SoundSourceSemantics::stampAuthoringAnchors(set.entries, previous, resolveAsset);
        };
        prepareSet(newState.sources.plain, oldState.sources.plain);
        for (auto it = newState.sources.localized.begin(); it != newState.sources.localized.end(); ++it)
            prepareSet(it.value(), oldState.sources.localized.value(it.key()));
        canonicalValue = SoundSourceSemantics::toJson(newState.sources);
    }

    ActionResult result;
    result.success = true;
    result.data = QJsonObject{
        {QStringLiteral("name"), item->name},
        {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
        {QStringLiteral("property"), propertyName},
        {QStringLiteral("value"), canonicalValue}};
    if (newState.sources == oldState.sources
        && newState.scheduleOffsetSec == oldState.scheduleOffsetSec
        && newState.distinguishLanguage == oldState.distinguishLanguage)
        return result;
    applySoundAuthoringState(item->objectID, newState, propertyName, originator);
    const uint32_t objectID = item->objectID;
    result.undoFn = [objectID, oldState, propertyName]() {
        applySoundAuthoringState(objectID, oldState, propertyName);
    };
    result.redoFn = [objectID, newState, propertyName]() {
        applySoundAuthoringState(objectID, newState, propertyName);
    };
    result.undoText = QStringLiteral("Set %1").arg(propertyName);
    return result;
}

bool claimsColorItemProperty(ProjectItem* , const QString& propertyName)
{
    return propertyName == QStringLiteral("color");
}

ActionResult handleColorItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                     const QString& propertyName, const QJsonObject& args,
                                     const QObject* originator)
{
    Q_UNUSED(projectManager);
    Q_UNUSED(originator);
    if (!AuthoringPolicy::supportsObjectColorEditing())
        return {false, {}, "COLOR_EDITING_UNAVAILABLE", "Object color editing is not available."};
    const uint32_t color = static_cast<uint32_t>(args["value"].toVariant().toLongLong());
    ProjectManager::instance().setItemColor(item, color);

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", propertyName},
        {"value", args["value"]}
    };
    return r;
}

bool claimsNotesItemProperty(ProjectItem* , const QString& propertyName)
{
    return propertyName == QStringLiteral("notes");
}

ActionResult handleNotesItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                     const QString& propertyName, const QJsonObject& args,
                                     const QObject* originator)
{
    const QString newNotes = args["value"].toString();
    const QString oldNotes = item->notes;

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", propertyName},
        {"value", newNotes}
    };
    if (newNotes == oldNotes)
        return r;

    item->notes = newNotes;
    item->markDirty();
    emit projectManager->itemPropertyChanged(item, propertyName, originator);

    auto applyFn = [](uint32_t objID, const QString& notes) {
        auto& pm = ProjectManager::instance();
        auto* proj = pm.currentProject();
        if (!proj) return;
        ProjectItem* pi = findItemByObjectIDInProject(proj, objID);
        if (!pi) pi = pm.findBusItem(objID);
        if (!pi) return;
        pi->notes = notes;
        pi->markDirty();
        emit pm.itemPropertyChanged(pi, QStringLiteral("notes"));
    };
    const uint32_t objID = item->objectID;
    r.undoFn = [applyFn, objID, oldNotes]() { applyFn(objID, oldNotes); };
    r.redoFn = [applyFn, objID, newNotes]() { applyFn(objID, newNotes); };
    r.undoText = QObject::tr("Edit Notes");
    return r;
}

namespace {

struct VariableDeclarationState {
    VariableKind kind;
    float rangeMin;
    float rangeMax;
    float defaultValue;
    std::vector<VariableLabel> labels;
};

VariableDeclarationState captureVariableDeclaration(const VVariable& var)
{
    return {var.kind, var.rangeMin, var.rangeMax, var.defaultValue, var.labels};
}

void assignVariableDeclaration(VVariable& var, const VariableDeclarationState& state)
{
    var.kind = state.kind;
    var.rangeMin = state.rangeMin;
    var.rangeMax = state.rangeMax;
    var.defaultValue = state.defaultValue;
    var.labels = state.labels;
}

QJsonArray invalidDiscreteGridCellDetails(
    ProjectManager* projectManager,
    const std::vector<InvalidDiscreteGridCell>& invalidCells)
{
    QJsonArray details;
    for (const InvalidDiscreteGridCell& invalid : invalidCells) {
        ProjectItem* host = projectManager->findItemByGuid(invalid.hostGuid);
        const auto* selection = host ? host->comp<VSelection>() : nullptr;
        ProjectItem* target = nullptr;
        if (selection && invalid.rowIndex >= 0
            && invalid.rowIndex
                < static_cast<int>(selection->matchGrid(invalid.platform).rows.size())) {
            target = projectManager->findItemByGuid(
                selection->matchGrid(invalid.platform).rows[invalid.rowIndex].target);
        }
        details.append(QJsonObject{
            {QStringLiteral("grid_name"),
             host ? host->name : QObject::tr("Missing Grid")},
            {QStringLiteral("grid_guid"),
             invalid.hostGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("row"), invalid.rowIndex + 1},
            {QStringLiteral("row_name"),
             target ? target->name : QObject::tr("Missing target")},
            {QStringLiteral("column"), invalid.axisIndex + 1},
            {QStringLiteral("value"), invalid.value},
        });
    }
    return details;
}

bool variableLabelsEqual(const std::vector<VariableLabel>& a, const std::vector<VariableLabel>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].anchor != b[i].anchor || a[i].name != b[i].name || a[i].id != b[i].id) return false;
    return true;
}

QStringList diffVariableDeclarations(const VariableDeclarationState& a,
                                     const VariableDeclarationState& b)
{
    QStringList out;
    if (a.kind != b.kind)
        out << QStringLiteral("kind");
    if (!qFuzzyCompare(a.rangeMin + 1.0f, b.rangeMin + 1.0f))
        out << QStringLiteral("rangeMin");
    if (!qFuzzyCompare(a.rangeMax + 1.0f, b.rangeMax + 1.0f))
        out << QStringLiteral("rangeMax");
    if (!qFuzzyCompare(a.defaultValue + 1.0f, b.defaultValue + 1.0f))
        out << QStringLiteral("defaultValue");
    if (!variableLabelsEqual(a.labels, b.labels))
        out << QStringLiteral("labels");
    return out;
}

void applyVariableDeclaration(uint32_t objectID, const VariableDeclarationState& state,
                              const QStringList& changedFields)
{
    auto& pm = ProjectManager::instance();
    ProjectData* proj = pm.currentProject();
    if (!proj) return;
    ProjectItem* pi = findItemByObjectIDInProject(proj, objectID);
    if (!pi) pi = pm.findBusItem(objectID);
    auto* var = pi ? pi->comp<VVariable>() : nullptr;
    if (!var) return;

    assignVariableDeclaration(*var, state);
    pi->markDirty();
    for (const QString& field : changedFields)
        emit pm.itemPropertyChanged(pi, field);
}

}

bool claimsVariableItemProperty(ProjectItem* item, const QString& propertyName)
{
    static const QSet<QString> kDeclarationProps = {
        QStringLiteral("kind"), QStringLiteral("rangeMin"), QStringLiteral("rangeMax"),
        QStringLiteral("range"), QStringLiteral("defaultValue"),
        QStringLiteral("labels")};
    return item->comp<VVariable>() != nullptr && kDeclarationProps.contains(propertyName);
}

ActionResult handleVariableItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                        const QString& propertyName, const QJsonObject& args,
                                        const QObject* originator)
{
    auto* var = item->comp<VVariable>();
    const VariableDeclarationState oldState = captureVariableDeclaration(*var);

    if (!var->setField(propertyName, args["value"].toVariant()))
    {
        ActionResult failed;
        failed.errorCode = QStringLiteral("SET_PROPERTY_FAILED");
        failed.errorMessage = QString("Failed to set '%1' on '%2'").arg(propertyName, item->name);
        return failed;
    }

    const VariableDeclarationState newState = captureVariableDeclaration(*var);

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", propertyName},
        {"value", args["value"]}
    };

    const QStringList changedFields = diffVariableDeclarations(oldState, newState);
    if (changedFields.isEmpty())
        return r;

    if (changedFields.contains(QStringLiteral("labels"))) {
        for (const auto& consumer : VariableConsumerSemantics::consumersOf(
                 projectManager->currentProject(), item->guid)) {
            if (consumer.kind != VariableConsumerKind::DiscreteGridAxis) continue;
            const auto* host = projectManager->findItemByGuid(consumer.hostGuid);
            const auto* selection = host ? host->comp<VSelection>() : nullptr;
            if (!selection) continue;
            for (int row = 0; row < static_cast<int>(selection->matchGrid(consumer.platform).rows.size()); ++row) {
                const auto cell = selection->matchGrid(consumer.platform).cellAt(row, consumer.axisIndex);
                if (!cell) continue;
                const auto* previous = cell->labelId.isNull()
                    ? VariableSemantics::labelAtIndex(oldState.labels, cell->legacyValue)
                    : VariableSemantics::labelById(oldState.labels, cell->labelId);
                if (previous && !VariableSemantics::labelById(newState.labels, previous->id)) {
                    assignVariableDeclaration(*var, oldState);
                    return {false, {}, "VARIABLE_LABEL_REFERENCED",
                        QObject::tr("Label '%1' is referenced by Grid '%2', row %3.")
                            .arg(previous->name.isEmpty() ? QString::number(previous->anchor) : previous->name,
                                 host->name).arg(row + 1)};
                }
            }
        }
    }

    if (changedFields.contains(QStringLiteral("rangeMin"))
        || changedFields.contains(QStringLiteral("rangeMax"))) {
        const auto invalidCells = VariableConsumerSemantics::invalidDiscreteGridCells(
            projectManager->currentProject(), item->guid,
            newState.rangeMin, newState.rangeMax);
        const QJsonArray invalidReferences =
            invalidDiscreteGridCellDetails(projectManager, invalidCells);
        r.data[QStringLiteral("range_min")] = newState.rangeMin;
        r.data[QStringLiteral("range_max")] = newState.rangeMax;
        r.data[QStringLiteral("invalid_reference_count")] =
            static_cast<qint64>(invalidCells.size());
        r.data[QStringLiteral("invalid_references")] = invalidReferences;
        if (!invalidCells.empty()
            && !args.value(QStringLiteral("allow_invalid_references")).toBool()) {
            assignVariableDeclaration(*var, oldState);
            ActionResult failed;
            failed.errorCode = QStringLiteral("VARIABLE_RANGE_CONFLICT");
            failed.errorMessage = QObject::tr(
                "Range [%1, %2] would make %3 discrete Grid cell(s) unreachable.")
                .arg(newState.rangeMin)
                .arg(newState.rangeMax)
                .arg(static_cast<qulonglong>(invalidCells.size()));
            failed.data = r.data;
            return failed;
        }
    }

    item->markDirty();
    for (const QString& field : changedFields)
        emit projectManager->itemPropertyChanged(item, field,
                                                 field == propertyName ? originator : nullptr);

    const uint32_t objID = item->objectID;
    r.undoFn = [objID, oldState, changedFields]() {
        applyVariableDeclaration(objID, oldState, changedFields);
    };
    r.redoFn = [objID, newState, changedFields]() {
        applyVariableDeclaration(objID, newState, changedFields);
    };
    r.undoText = QString("Set %1").arg(propertyName);
    return r;
}

bool claimsSelectionItemProperty(ProjectItem* item, const QString& propertyName)
{
    return (propertyName == QStringLiteral("mode") || propertyName == QStringLiteral("behavior")
            || propertyName == QStringLiteral("playType")
            || propertyName == QStringLiteral("variableResponse")
            || propertyName == QStringLiteral("selections")
            || propertyName == QStringLiteral("spliceByMarker")
            || propertyName == QStringLiteral("transitions"))
        && item->comp<VSelection>() != nullptr;
}

bool claimsTimelineItemProperty(ProjectItem* item, const QString& propertyName)
{
    return (propertyName == QStringLiteral("clips")
            && item->comp<VTrack>() != nullptr)
        || (propertyName == QStringLiteral("pieceTime")
            && item->comp<VPiece>() != nullptr);
}

bool claimsEffectsItemProperty(ProjectItem* item, const QString& propertyName)
{
    return propertyName == QStringLiteral("effects")
        && item->comp<VMixNode>() != nullptr;
}

namespace {

bool gridAxesAreUnique(const std::vector<QUuid>& axes)
{
    for (size_t i = 0; i < axes.size(); ++i)
        for (size_t j = i + 1; j < axes.size(); ++j)
            if (axes[i] == axes[j])
                return false;
    return true;
}

bool finiteNumber(const QJsonValue& value)
{
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool validGridTableJson(ProjectManager* projectManager, ProjectItem* owner,
                        const QJsonObject& table)
{
    const QJsonValue axesValue = table.value(QStringLiteral("axes"));
    const QJsonValue rowsValue = table.value(QStringLiteral("rows"));
    if ((!axesValue.isUndefined() && !axesValue.isArray())
        || (!rowsValue.isUndefined() && !rowsValue.isArray())) return false;
    const QJsonArray axes = axesValue.toArray();
    for (const QJsonValue& axisValue : axes) {
        if (!axisValue.isString()) return false;
        const QUuid axis = QUuid::fromString(axisValue.toString());
        const ProjectItem* variable = projectManager->findItemByGuid(axis);
        if (axis.isNull() || !variable
            || variable->type != ProjectItem::Type::Variable) return false;
    }
    for (const QJsonValue& rowValue : rowsValue.toArray()) {
        if (!rowValue.isObject()) return false;
        const QJsonObject row = rowValue.toObject();
        if (!row.value(QStringLiteral("target")).isString()) return false;
        const QUuid target = QUuid::fromString(
            row.value(QStringLiteral("target")).toString());
        if (!GridSemantics::isGridTargetCandidate(*owner, target)) return false;
        const QJsonValue cellsValue = row.value(QStringLiteral("cells"));
        if (!cellsValue.isArray()) return false;
        const QJsonArray cells = cellsValue.toArray();
        if (cells.size() != axes.size()) return false;
        for (const QJsonValue& cell : cells) {
            if (!cell.isNull()) {
                if (cell.isObject()) {
                    const auto reference = cell.toObject();
                    if (reference.size() != 1
                        || QUuid(reference.value(QStringLiteral("labelId")).toString()).isNull()) return false;
                    continue;
                }
                if (!finiteNumber(cell)) return false;
                const double number = cell.toDouble();
                if (std::trunc(number) != number
                    || number < 0
                    || number > VariableSemantics::kStepExactIntBound) return false;
            }
        }
    }
    return true;
}

TimelineValidation validateSelectionActivation(const ProjectItem& item, const VSelection& candidate)
{
    auto result = AuthoringPolicy::validateTransitionActivation(item, candidate);
    if (!result) return result;
    QSet<QString> platforms;
    for (const auto& key : candidate.selections.platforms.keys()) platforms.insert(key);
    if (const auto* previous = item.comp<VSelection>())
        for (const auto& key : previous->selections.platforms.keys()) platforms.insert(key);
    for (const auto& key : platforms) {
        result = AuthoringPolicy::validateTransitionActivation(item, candidate, QUuid(key));
        if (!result) return result;
    }
    return TimelineValidation::success();
}

void applySelectionTransitions(uint32_t objectID,
                               const TransitionTableDef& transitions)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* item = pm.findItemByObjectID(objectID);
    auto* selection = item ? item->comp<VSelection>() : nullptr;
    if (!selection) return;
    selection->transitions = transitions;
    item->markDirty();
    emit pm.itemPropertyChanged(item, QStringLiteral("transitions"));
}

ActionResult handleSelectionTransitionsProperty(
    ProjectManager* projectManager, ProjectItem* item,
    const QJsonObject& args, const QObject* originator)
{
    const QJsonValue value = args.value(QStringLiteral("value"));
    if (!value.isObject())
        return {false, {}, "INVALID_ARGS",
                "Property 'transitions' value must be a JSON object"};

    QString parseError;
    auto parsed = TransitionTableDef::fromJson(value.toObject(), &parseError);
    if (!parsed)
        return {false, {}, "INVALID_ARGS", parseError};

    auto* selection = item->comp<VSelection>();
    const TransitionTableDef oldTransitions = selection->transitions;
    const auto authoring = AuthoringPolicy::validateTransitions(*parsed, &oldTransitions);
    if (!authoring)
        return {false, {}, "AUTHORING_RESTRICTED", authoring.message};

    for (size_t index = 0; index < parsed->rules.size(); ++index) {
        const TransitionRuleDef& rule = parsed->rules[index];
        const auto previous = std::find_if(oldTransitions.rules.begin(), oldTransitions.rules.end(),
            [&](const auto& old) { return old.source == rule.source && old.target == rule.target; });
        const bool retained = previous != oldTransitions.rules.end() && *previous == rule;
        const bool repaired = previous != oldTransitions.rules.end()
            && !AuthoringPolicy::allowsTransitionExitTiming(previous->exitTiming)
            && AuthoringPolicy::allowsTransitionExitTiming(rule.exitTiming);
        if (!retained)
            for (const auto* reference : {&rule.source, &rule.target, &rule.bridge}) {
                if (!*reference) continue;
                if (repaired && (reference != &rule.bridge || rule.bridge == previous->bridge))
                    continue;
                const auto validity = validateAuthoredReference(
                    projectManager, projectManager->findItemByGuid(**reference));
                if (!validity)
                    return {false, {}, "AUTHORING_RESTRICTED", validity.message};
            }
        for (const auto* endpoint : {&rule.source, &rule.target}) {
            if (!*endpoint) continue;
            const ProjectItem* candidate = projectManager->findItemByGuid(**endpoint);
            if (!candidate
                || !TransitionSemantics::isEndpointCandidate(*item, *candidate)) {
                return {false, {}, "INVALID_TARGET",
                        QStringLiteral("Owner '%1' (%2), Transition rule %3 endpoint %4: %5")
                            .arg(item->name, item->guid.toString(QUuid::WithoutBraces))
                            .arg(index).arg((**endpoint).toString(QUuid::WithoutBraces),
                                candidate ? QStringLiteral("target is not a legal direct child")
                                          : QStringLiteral("target does not exist"))};
            }
        }
        if (rule.bridge) {
            const ProjectItem* bridge = projectManager->findItemByGuid(*rule.bridge);
            if (bridge && !TransitionSemantics::isBridgeCandidate(*bridge)) {
                return {false, {}, "INVALID_TARGET",
                        QStringLiteral("Transition rule %1 bridge is not a content object")
                            .arg(index)};
            }
        }
    }

    const QJsonObject oldJson = oldTransitions.toJson();
    const QJsonObject newJson = parsed->toJson();

    ActionResult result;
    result.success = true;
    result.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", QStringLiteral("transitions")},
        {"value", newJson}
    };
    if (newJson == oldJson)
        return result;

    selection->transitions = *parsed;
    item->markDirty();
    emit projectManager->itemPropertyChanged(
        item, QStringLiteral("transitions"), originator);

    const uint32_t objectID = item->objectID;
    const TransitionTableDef newTransitions = std::move(*parsed);
    result.undoFn = [objectID, oldTransitions]() {
        applySelectionTransitions(objectID, oldTransitions);
    };
    result.redoFn = [objectID, newTransitions]() {
        applySelectionTransitions(objectID, newTransitions);
    };
    const QString undoText = args.value(QStringLiteral("undo_text")).toString();
    result.undoText = undoText.isEmpty()
        ? QStringLiteral("Set transitions") : undoText;
    return result;
}


}

void applySelectionBehavior(uint32_t objectID, const SelectionBehaviorDef& behavior)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* item = manager.findItemByObjectID(objectID);
    auto* selection = item ? item->comp<VSelection>() : nullptr;
    if (!selection) return;
    selection->behavior = behavior;
    item->markDirty();
    emit manager.itemPropertyChanged(item, QStringLiteral("behavior"));
}

ActionResult handleSelectionBehaviorProperty(ProjectManager* projectManager,
                                             ProjectItem* item,
                                             const QJsonObject& args,
                                             const QObject* originator)
{
    const QJsonValue value = args.value(QStringLiteral("value"));
    if (!value.isObject())
        return {false, {}, "INVALID_ARGS", "Property 'behavior' value must be an object"};
    auto* selection = item->comp<VSelection>();
    QString error;
    const auto parsed = ContainerBehaviorSemantics::fromJson(value.toObject(), &error);
    if (!parsed) return {false, {}, "INVALID_ARGS", error};
    VSelection candidate = *selection;
    candidate.behavior = *parsed;
    if (!ContainerBehaviorSemantics::validateSelections(candidate, &error))
        return {false, {}, "INVALID_ARGS", error};
    const SelectionBehaviorDef oldBehavior = selection->behavior;
    const QJsonObject canonical = ContainerBehaviorSemantics::toJson(*parsed);
    ActionResult result;
    result.success = true;
    result.data = QJsonObject{
        {QStringLiteral("name"), item->name},
        {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
        {QStringLiteral("property"), QStringLiteral("behavior")},
        {QStringLiteral("value"), canonical}};
    if (*parsed == oldBehavior) return result;
    selection->behavior = *parsed;
    item->markDirty();
    emit projectManager->itemPropertyChanged(item, QStringLiteral("behavior"), originator);
    const uint32_t objectID = item->objectID;
    result.undoFn = [objectID, oldBehavior]() {
        applySelectionBehavior(objectID, oldBehavior);
    };
    result.redoFn = [objectID, newBehavior = *parsed]() {
        applySelectionBehavior(objectID, newBehavior);
    };
    result.undoText = QStringLiteral("Set behavior");
    return result;
}

ActionResult handleSelectionsProperty(ProjectManager* manager, ProjectItem* item,
                                      const QJsonObject& args, const QObject* originator)
{
    if (!args.value(QStringLiteral("value")).isObject())
        return {false, {}, "INVALID_ARGS", "Selections value must be an object"};
    QString error;
    auto parsed = SelectionData::fromJson(args.value(QStringLiteral("value")).toObject(), &error);
    if (!parsed) return {false, {}, "INVALID_SELECTIONS", error};
    if (!ProjectPlatformSemantics::validateIds(parsed->platforms.keys(),
            manager->currentProject()->platforms, &error))
        return {false, {}, "INVALID_SELECTIONS", error};
    const auto* selection = item->comp<VSelection>();
    const auto previousTables = selection->selections.tables();
    QHash<QUuid, std::vector<VariableLabel>> oldLabels;
    QHash<QUuid, std::vector<VariableLabel>> newLabels;
    for (auto* table : parsed->tables()) {
        if (!PlayablePropertyAuthoringV1::validateContainerOccurrenceTargets(*item, *table, &error))
            return {false, {}, "INVALID_CHILD_REFERENCE", error};
        const auto grid = table->matchGrid();
        if (!validGridTableJson(manager, item, grid.toJson()) || !gridAxesAreUnique(grid.axes))
            return {false, {}, "INVALID_SELECTIONS", "Invalid selection conditions"};
        for (size_t axis = 0; axis < table->matchAxes.size(); ++axis) {
            const auto variableGuid = table->matchAxes[axis];
            auto* variableItem = manager->findItemByGuid(variableGuid);
            auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
            if (!variable) return {false, {}, "INVALID_TARGET", "Selection axis has no Variable"};
            auto labels = newLabels.value(variableGuid, variable->labels);
            for (auto& entry : table->entries) {
                for (auto& condition : entry.conditions) {
                    auto& cell = condition.cells[axis];
                    if (!cell) continue;
                    if (!cell->labelId.isNull()) {
                        if (!VariableSemantics::labelById(labels, cell->labelId))
                            return {false, {}, "INVALID_VARIABLE_LABEL", "Selection Label does not belong to its Variable"};
                        continue;
                    }
                    const int value = cell->legacyValue;
                    if (value < variable->rangeMin || value > variable->rangeMax)
                        return {false, {}, "INVALID_VARIABLE_LABEL", "Selection value is outside its Variable range"};
                    const auto* label = VariableSemantics::labelAtIndex(labels, value);
                    if (!label) {
                        labels.push_back({value, {}});
                        label = &labels.back();
                    }
                    cell = DiscreteGridCell(label->id);
                }
            }
            if (!variableLabelsEqual(labels, variable->labels)) {
                std::sort(labels.begin(), labels.end(), [](const auto& a, const auto& b) { return a.anchor < b.anchor; });
                oldLabels.insert(variableGuid, variable->labels);
                newLabels.insert(variableGuid, std::move(labels));
            }
        }
        for (const auto& row : table->mixingRows) {
            if (row.variable.isNull()) continue;
            const auto* variable = manager->findItemByGuid(row.variable);
            if (!variable || !variable->comp<VVariable>())
                return {false, {}, "INVALID_TARGET", "Selection curve axis has no Variable"};
        }
        for (const auto& row : table->occurrenceRows()) {
            const bool existing = std::any_of(previousTables.cbegin(), previousTables.cend(), [&](const auto* previous) {
                const auto* member = previous->memberForOccurrence(row.stableOccurrenceOrdinal);
                return member && member->childGuid == row.childGuid;
            });
            if (existing) continue;
            const auto* child = manager->findItemByGuid(row.childGuid);
            const auto authoring = AuthoringPolicy::validateNewSubtree(*child, nullptr,
                [manager](const QUuid& guid) { return manager->findItemByGuid(guid); },
                [manager](uint32_t id) { return manager->findItemByObjectID(id); });
            if (!authoring) return {false, {}, "AUTHORING_RESTRICTED", authoring.message};
        }
    }
    VSelection candidate = *selection;
    candidate.selections = *parsed;
    if (!ContainerBehaviorSemantics::validateSelections(candidate, &error))
        return {false, {}, "INVALID_SELECTIONS", error};
    const auto activation = validateSelectionActivation(*item, candidate);
    if (!activation) return {false, {}, "AUTHORING_RESTRICTED", activation.message};
    const auto before = selection->selections;
    const auto after = *parsed;
    ActionResult result;
    result.success = true;
    result.data = {{QStringLiteral("property"), QStringLiteral("selections")},
                   {QStringLiteral("value"), after.toJson()}};
    if (before == after && newLabels.isEmpty()) return result;
    const auto apply = [manager, id = item->objectID](const SelectionData& value,
            const QHash<QUuid, std::vector<VariableLabel>>& labels, const QObject* source) {
        auto* owner = manager->findItemByObjectID(id);
        auto* selection = owner ? owner->comp<VSelection>() : nullptr;
        if (!selection) return;
        for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
            auto* variableItem = manager->findItemByGuid(it.key());
            auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
            if (!variable) continue;
            variable->labels = it.value();
            variableItem->markDirty();
        }
        selection->selections = value;
        owner->markDirty();
        emit manager->itemPropertyChanged(owner, QStringLiteral("selections"), source);
        for (auto it = labels.cbegin(); it != labels.cend(); ++it)
            if (auto* variableItem = manager->findItemByGuid(it.key()))
                emit manager->itemPropertyChanged(variableItem, QStringLiteral("labels"));
    };
    apply(after, newLabels, originator);
    result.undoFn = [apply, before, oldLabels] { apply(before, oldLabels, nullptr); };
    result.redoFn = [apply, after, newLabels] { apply(after, newLabels, nullptr); };
    result.undoText = args.value(QStringLiteral("undo_text")).toString(QStringLiteral("Edit Selections"));
    return result;
}

ActionResult handleSelectionItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                         const QString& propertyName, const QJsonObject& args,
                                         const QObject* originator)
{
    if (propertyName == QStringLiteral("selections"))
        return handleSelectionsProperty(projectManager, item, args, originator);
    if (propertyName == QStringLiteral("behavior"))
        return handleSelectionBehaviorProperty(projectManager, item, args, originator);
    if (propertyName == QStringLiteral("spliceByMarker")) {
        const QJsonValue value = args.value(QStringLiteral("value"));
        const auto* selection = item->comp<VSelection>();
        if (item->type != ProjectItem::Type::Container
            || !selection->isQueue())
            return {false, {}, "INVALID_TIMELINE_OWNER",
                    "spliceByMarker requires a Queue Container"};
        if (!value.isBool())
            return {false, {}, "INVALID_ARGS",
                    "Property 'spliceByMarker' value must be a boolean"};
        return TimelineEditing::setSpliceByMarker(
            projectManager, item, value.toBool(), originator,
            args.value(QStringLiteral("undo_text")).toString());
    }
    if (propertyName == QStringLiteral("transitions"))
        return handleSelectionTransitionsProperty(projectManager, item, args, originator);

    auto* sel = item->comp<VSelection>();
    const VSelection previous = *sel;
    VSelection candidate = *sel;

    if (!candidate.setField(propertyName, args["value"].toVariant()))
    {
        ActionResult failed;
        failed.errorCode = QStringLiteral("SET_PROPERTY_FAILED");
        failed.errorMessage = QString("Failed to set '%1' on '%2'").arg(propertyName, item->name);
        return failed;
    }
    const auto activation = validateSelectionActivation(*item, candidate);
    if (!activation)
        return {false, {}, "AUTHORING_RESTRICTED", activation.message};
    QString selectionError;
    if (!ContainerBehaviorSemantics::validateSelections(candidate, &selectionError))
        return {false, {}, "INVALID_SELECTIONS", selectionError};
    *sel = candidate;

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"name", item->name},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"property", propertyName},
        {"value", args["value"]}
    };
    if (sel->toJson() == previous.toJson())
        return r;

    item->markDirty();
    emit projectManager->itemPropertyChanged(item, propertyName, originator);

    auto applyFn = [propertyName](uint32_t objectID, const VSelection& value) {
        auto& pm = ProjectManager::instance();
        ProjectData* proj = pm.currentProject();
        if (!proj) return;
        ProjectItem* pi = findItemByObjectIDInProject(proj, objectID);
        if (!pi) pi = pm.findBusItem(objectID);
        auto* s = pi ? pi->comp<VSelection>() : nullptr;
        if (!s) return;
        *s = value;
        pi->markDirty();
        emit pm.itemPropertyChanged(pi, propertyName);
    };
    const uint32_t objID = item->objectID;
    r.undoFn = [applyFn, objID, previous]() { applyFn(objID, previous); };
    r.redoFn = [applyFn, objID, candidate]() { applyFn(objID, candidate); };
    r.undoText = QString("Set %1").arg(propertyName);
    return r;
}

ActionResult handleTimelineItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                        const QString& propertyName, const QJsonObject& args,
                                        const QObject* originator)
{
    const QJsonValue value = args.value(QStringLiteral("value"));
    const QString undoText = args.value(QStringLiteral("undo_text")).toString();
    if (propertyName == QStringLiteral("clips")) {
        if (!value.isObject())
            return {false, {}, "INVALID_ARGS",
                    "Property 'clips' value must be an exact ClipLedger object"};
        QString error;
        auto clips = ClipLedger::fromJson(value.toObject(), &error);
        if (!clips)
            return {false, {}, "INVALID_ARGS", error};
        return TimelineEditing::replaceClips(
            projectManager, item, *clips, originator, undoText);
    }
    if (!value.isObject())
        return {false, {}, "INVALID_ARGS",
                "Property 'pieceTime' value must be a JSON object"};
    const auto authoring = AuthoringPolicy::validatePieceTime(value.toObject());
    if (!authoring)
        return {false, {}, "AUTHORING_RESTRICTED", authoring.message};
    QString error;
    auto pieceTime = PieceTimeSpec::fromJson(value.toObject(), &error);
    if (!pieceTime)
        return {false, {}, "INVALID_ARGS", error};
    return TimelineEditing::replacePieceTime(
        projectManager, item, *pieceTime, originator, undoText);
}

ActionResult handleEffectsItemProperty(ProjectManager* projectManager, ProjectItem* item,
                                       const QString&, const QJsonObject& args,
                                       const QObject* originator)
{
    if (!args.value(QStringLiteral("value")).isArray())
        return {false, {}, "INVALID_ARGS", "'value' must be an effects array"};
    EffectChainLedger parsed = EffectChainLedger::fromJson(
        args.value(QStringLiteral("value")).toArray());
    std::vector<EffectEntry> entries = parsed.entries();
    for (EffectEntry& effect : entries)
        if (effect.typed && harkenstudio::IsNull(effect.runtimeId()))
            effect.guid = QUuid::createUuid();
    EffectChainLedger checked;
    if (!checked.replace(std::move(entries)))
        return {false, {}, "INVALID_ARGS", "Invalid typed effect chain"};
    auto* mix = item->comp<VMixNode>();
    const QJsonArray oldValue = mix->effects.toJson();
    const QJsonArray newValue = checked.toJson();
    ActionResult result;
    result.success = true;
    result.data = QJsonObject{{"property", QStringLiteral("effects")},
                              {"value", newValue}};
    if (oldValue == newValue) return result;
    const QUuid guid = item->guid;
    auto apply = [guid, originator](const QJsonArray& value) {
        ProjectItem* target = ProjectManager::instance().findItemByGuid(guid);
        auto* targetMix = target ? target->comp<VMixNode>() : nullptr;
        if (!targetMix) return;
        targetMix->effects = EffectChainLedger::fromJson(value);
        target->markDirty();
        emit ProjectManager::instance().itemPropertyChanged(
            target, QStringLiteral("effects"), originator);
        ProjectManager::instance().requestResync(target);
    };
    apply(newValue);
    result.undoFn = [apply, oldValue]() { apply(oldValue); };
    result.redoFn = [apply, newValue]() { apply(newValue); };
    result.undoText = QStringLiteral("Set effects");
    return result;
}

namespace {

bool claimsAttenuationItemProperty(ProjectItem* item, const QString& propertyName)
{
    return item->comp<VAttenuation>()
        && (propertyName == QLatin1String("dimensions") || propertyName == QLatin1String("maxDistance"));
}

ActionResult handleAttenuationItemProperty(ProjectManager* manager, ProjectItem* item,
                                         const QString& propertyName, const QJsonObject& args,
                                         const QObject* originator)
{
    auto result = AttenuationStore::edit(manager, *item->comp<VAttenuation>(),
        {{propertyName, args.value("value")}}, QStringLiteral("Set %1").arg(propertyName), originator);
    if (result.success) {
        auto data = result.data;
        data.insert("property", propertyName);
        result.data = data;
    }
    return result;
}

struct ItemPropertyRoute {
    bool (*claims)(ProjectItem*, const QString&);
    ActionResult (*handle)(ProjectManager*, ProjectItem*, const QString&, const QJsonObject&,
                           const QObject*);
};

const ItemPropertyRoute kItemPropertyRoutes[] = {
    {claimsPlayableItemProperty,      handlePlayableItemProperty},
    {claimsSoundItemProperty,       handleSoundItemProperty},
    {claimsColorItemProperty,         handleColorItemProperty},
    {claimsNotesItemProperty,         handleNotesItemProperty},
    {claimsStepsItemProperty,         handleStepsItemProperty},
    {claimsBusItemProperty,           handleBusItemProperty},
    {claimsVariableItemProperty,      handleVariableItemProperty},
    {claimsAttenuationItemProperty,   handleAttenuationItemProperty},
    {claimsTimelineItemProperty,      handleTimelineItemProperty},
    {claimsSelectionItemProperty,     handleSelectionItemProperty},
    {claimsEffectsItemProperty,       handleEffectsItemProperty},
    {claimsAudioItemProperty,         handleAudioItemProperty},
};

}

void registerProjectActions(ActionManager& mgr,
                           ProjectManager* projectManager,
                           ProjectExplorer* projectExplorer,
                           std::function<Hierarchy*()> hierarchyResolver)
{
    registerProjectPlatformActions(mgr, projectManager);
    registerFieldGroupActions(mgr, projectManager);
    {
        ActionDef def;
        def.id = "save_project";
        def.displayName = "Save Project";
        def.category = "project";
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            Q_UNUSED(args);
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            bool success = projectManager->saveProject();
            ActionResult r;
            r.success = success;
            r.data = QJsonObject{{"success", success}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "create_project";
        def.displayName = "Create Project";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"name", "string"}, {"location", "string"}, {"sample_rate", "int"}, {"author", "string"}, {"description", "string"}, {"enable_lfe", "bool"}, {"channel_config", "string"}, {"platforms", "array"}, {"template_id", "string"}, {"template_data", "object"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE",
                        "ProjectManager is not available"};

            ProjectConfig config;
            config.name        = args.value("name").toString();
            config.location    = args.value("location").toString();
            if (config.name.isEmpty() || config.location.isEmpty())
                return {false, {}, "INVALID_ARGS",
                        "Required: name, location"};

            config.sampleRate  = args.value("sample_rate").toInt(48000);
            config.author      = args.value("author").toString();
            config.description = args.value("description").toString();
            config.enableLFE   = args.value("enable_lfe").toBool(true);

            const QString cc = args.value("channel_config").toString().toLower();
            if      (cc == QStringLiteral("mono"))           config.channelConfig = AudioChannelConfig::Mono;
            else if (cc == QStringLiteral("5_1"))            config.channelConfig = AudioChannelConfig::Surround_5_1;
            else if (cc == QStringLiteral("7_1"))            config.channelConfig = AudioChannelConfig::Surround_7_1;
            else if (cc == QStringLiteral("atmos_7_1_4"))    config.channelConfig = AudioChannelConfig::Atmos_7_1_4;
            else                                              config.channelConfig = AudioChannelConfig::Stereo_2_0;

            if (args.contains("target_platforms"))
                return {false, {}, "UNSUPPORTED_PLATFORM_FORMAT", "Use platform instance definitions in 'platforms'"};
            QString platformError;
            if (args.contains("platforms")
                && !ProjectPlatformSemantics::platformsFromJson(args.value("platforms"), &config.platforms, &platformError))
                return {false, {}, "INVALID_ARGS", platformError};

            config.templateId   = args.value("template_id").toString();
            if (args.value("template_data").isObject()) {
                config.templateData = args.value("template_data").toObject();
            } else if (args.value("template_data").isString()) {
                const auto doc = QJsonDocument::fromJson(
                    args.value("template_data").toString().toUtf8());
                if (doc.isObject())
                    config.templateData = doc.object();
            }

            if (!projectManager->createProject(config))
                return {false, {}, "CREATE_PROJECT_FAILED",
                        "Failed to create project (check location is writable)"};

            ProjectData* p = projectManager->currentProject();
            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success",          true},
                {"path",             p ? p->projectPath : QString()},
                {"template_applied", !config.templateId.isEmpty()
                                     && !config.templateData.isEmpty()}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "close_project";
        def.displayName = "Close Project";
        def.category = "project";
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            Q_UNUSED(args);
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};

            QTimer::singleShot(0, projectManager, [pm = projectManager]() {
                pm->closeProject();
            });

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"success", true}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "create_pack";
        def.displayName = "Create Pack";
        def.category = "project";
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};

            Q_UNUSED(args);
            return {false, {}, "ACTION_DEPRECATED",
                    "create_pack is no longer supported; use create_item with the desired ProjectItem type instead"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "rename_pack";
        def.displayName = "Rename Pack";
        def.category = "project";
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            Q_UNUSED(projectManager);
            Q_UNUSED(args);
            return {false, {}, "ACTION_DEPRECATED",
                    "rename_pack is no longer supported; rename the underlying ProjectItem instead"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "delete_pack";
        def.displayName = "Delete Pack";
        def.category = "project";
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            Q_UNUSED(projectManager);
            Q_UNUSED(args);
            return {false, {}, "ACTION_DEPRECATED",
                    "delete_pack is no longer supported; delete the underlying ProjectItem instead"};
        };
        mgr.registerAction(def);
    }

    for (const QString& command : {QStringLiteral("create_folder"),
             QStringLiteral("rename_folder"), QStringLiteral("delete_folder"),
             QStringLiteral("move_folder"), QStringLiteral("get_folder_info"),
             QStringLiteral("list_folders"), QStringLiteral("select_folder")}) {
        ActionDef def;
        def.id = command;
        def.displayName = command;
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"path", "string"}};
        if (command == QLatin1String("rename_folder"))
            def.paramSchema.insert("new_name", "string");
        if (command == QLatin1String("move_folder"))
            def.paramSchema.insert("new_parent", "string");
        def.handler = [projectManager, projectExplorer, hierarchyResolver, command](
                         const QJsonObject& args) -> ActionResult {
            ProjectData* project = projectManager ? projectManager->currentProject() : nullptr;
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            const auto parsePath = [](const QString& value, QString* bucket,
                                      QString* relative) {
                const QStringList parts = QDir::fromNativeSeparators(value)
                    .split(QLatin1Char('/'), Qt::KeepEmptyParts);
                if (parts.isEmpty() || !ItemTypeCatalog::bucketAllowsFolder(parts.front()))
                    return false;
                for (const QString& part : parts) {
                    if (part.trimmed() != part || part.endsWith(QLatin1Char('.'))
                        || !NameValidator::validateFileName(part)) return false;
                }
                *bucket = parts.front();
                *relative = parts.mid(1).join(QLatin1Char('/'));
                return true;
            };
            QString bucket;
            QString relative;
            if (!parsePath(args.value("path").toString(), &bucket, &relative))
                return {false, {}, "INVALID_PATH", "path must start with a folder-enabled bucket"};
            const auto describe = [project, &bucket](const PhysicalFolder* folder) {
                return QJsonObject{{"name", folder->name}, {"bucket", bucket},
                    {"path", bucket + QLatin1Char('/') + project->folderRelPath(folder)}};
            };
            PhysicalFolder* folder = relative.isEmpty() ? nullptr
                : project->findFolderByPath(bucket, relative);
            if (command == QLatin1String("create_folder")) {
                if (relative.isEmpty())
                    return {false, {}, "INVALID_PATH", "A directory name is required after the bucket"};
                if (folder)
                    return {false, {}, "FOLDER_EXISTS", "The directory already exists"};
                const qsizetype slash = relative.lastIndexOf(QLatin1Char('/'));
                PhysicalFolder* parent = slash < 0 ? nullptr
                    : project->findFolderByPath(bucket, relative.left(slash));
                if (slash >= 0 && !parent)
                    return {false, {}, "PARENT_NOT_FOUND", "The parent directory does not exist"};
                folder = projectManager->createFolder(parent, relative.mid(slash + 1), bucket);
                if (!folder)
                    return {false, {}, "CREATE_FOLDER_FAILED", "Failed to create directory"};
                return {true, describe(folder)};
            }
            if (!folder && (!relative.isEmpty() || command != QLatin1String("list_folders")))
                return {false, {}, "FOLDER_NOT_FOUND", "The directory does not exist"};
            if (command == QLatin1String("list_folders")) {
                QJsonArray children;
                if (folder) {
                    for (const auto& child : folder->children) children.append(describe(child.get()));
                } else {
                    for (const PhysicalFolder* root : project->folderRootsForBucket(bucket))
                        children.append(describe(root));
                }
                return {true, {{"folders", children}}};
            }
            if (command == QLatin1String("get_folder_info")) {
                QJsonObject info = describe(folder);
                QJsonArray children;
                for (const auto& child : folder->children) children.append(describe(child.get()));
                QJsonArray items;
                for (const auto* binding : project->allDocumentBindings()) {
                    const auto* item = projectManager->findItemByGuid(binding->guid);
                    if (binding->folder == folder && item)
                        items.append(project->itemPath(item));
                }
                info.insert("folders", children);
                info.insert("items", items);
                return {true, info};
            }
            if (command == QLatin1String("rename_folder")) {
                QString error;
                const QString name = args.value("new_name").toString();
                if (!projectManager->validateFolderRename(folder, name, &error))
                    return {false, {}, "INVALID_NAME", error};
                if (!projectManager->renameFolder(folder, name))
                    return {false, {}, "RENAME_FAILED", "Failed to rename directory"};
                return {true, describe(folder)};
            }
            if (command == QLatin1String("delete_folder")) {
                const QString path = bucket + QLatin1Char('/') + project->folderRelPath(folder);
                if (!projectManager->deleteEntries({}, {folder}))
                    return {false, {}, "DELETE_FAILED", "The directory could not be deleted"};
                return {true, {{"deleted_path", path}}};
            }
            if (command == QLatin1String("move_folder")) {
                QString destinationBucket;
                QString destination;
                if (!parsePath(args.value("new_parent").toString(), &destinationBucket, &destination)
                    || destinationBucket != bucket)
                    return {false, {}, "INVALID_PATH", "new_parent must be in the same bucket"};
                PhysicalFolder* parent = destination.isEmpty() ? nullptr
                    : project->findFolderByPath(bucket, destination);
                if (!destination.isEmpty() && !parent)
                    return {false, {}, "PARENT_NOT_FOUND", "The parent directory does not exist"};
                if (!projectManager->moveFolder(folder, parent, bucket))
                    return {false, {}, "MOVE_FAILED", "Failed to move directory"};
                return {true, describe(folder)};
            }
            Hierarchy* hierarchy = hierarchyResolver ? hierarchyResolver() : nullptr;
            const bool explorerSelected = projectExplorer && projectExplorer->selectFolder(folder);
            const bool hierarchySelected = hierarchy && hierarchy->selectFolder(folder);
            if (!explorerSelected && !hierarchySelected)
                return {false, {}, "FOLDER_NOT_VISIBLE", "The directory is not visible in a project view"};
            QJsonObject selected = describe(folder);
            selected.insert("selected", true);
            return {true, selected};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "create_item";
        def.displayName = "Create Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"parent", "string"}, {"name", "string"}, {"type", "string"}, {"kind", "string"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString parentPath = args["parent"].toString();
            QString name = args["name"].toString();
            QString typeStr = args["type"].toString();

            if (name.isEmpty() || typeStr.isEmpty())
                return {false, {}, "INVALID_ARGS", "Item name and type are required"};

            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            ProjectItem* parent = nullptr;
            if (!parentPath.isEmpty()) {
                const auto selected = resolveProjectSelector(project, parentPath);
                if (!selected.ok)
                    return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
                parent = selected.item;
            }

            const auto parsedType = stringToItemType(typeStr);
            if (!parsedType)
                return {false, {}, "INVALID_TYPE",
                        QString("Unknown item type '%1'").arg(typeStr)};
            ProjectItem::Type type = *parsedType;

            if (type == ProjectItem::Type::Bus)
                return {false, {}, "USE_BUS_API",
                        "Use create_bus / harken_create_bus for bus creation, not create_item"};

            // create_item covers every menu-creatable type except Bus, which is
            // split off to create_bus by the guard above.
            static const QSet<ProjectItem::Type> kCreatableTypes = [] {
                QSet<ProjectItem::Type> types;
                for (const ItemTypeCatalog::CreationSpec& spec : ItemTypeCatalog::creationSpecs()) {
                    if (!spec.comingSoon)
                        types.insert(spec.type);
                }
                types.remove(ProjectItem::Type::Bus);
                return types;
            }();
            if (!kCreatableTypes.contains(type))
                return {false, {}, "TYPE_NOT_SUPPORTED",
                        QString("Item type '%1' cannot be created").arg(typeStr)};

            NameValidator::Result vr;
            if (!NameValidator::validateItemName(name, type, &vr))
                return {false, {}, "INVALID_NAME", NameValidator::resultMessage(vr)};

            VariableKind variableKind = VariableKind::Smooth;
            if (args.contains(QStringLiteral("kind"))) {
                const QString kind = args.value(QStringLiteral("kind")).toString();
                if (type != ProjectItem::Type::Variable
                    || (kind != QStringLiteral("Step") && kind != QStringLiteral("Smooth")))
                    return {false, {}, "INVALID_ARGS", "Variable kind must be Step or Smooth"};
                variableKind = kind == QStringLiteral("Step") ? VariableKind::Step : VariableKind::Smooth;
            }
            ProjectItem* item = projectManager->createItem(parent, name, type, {}, {}, nullptr, variableKind);
            if (item)
            {
                ActionResult r;
                r.success = true;
                r.data = QJsonObject{
                    {"success", true},
                    {"name", item->name},
                    {"type", itemTypeToString(item->type)},
                    {"object_id", static_cast<qint64>(itemShortID(item))},
                    {"authoring_handle", static_cast<qint64>(item->objectID)},
                    {"guid", item->guid.toString(QUuid::WithoutBraces)},
                    {"path", buildItemFullPath(project, item)}
                };
                return r;
            }
            else
            {
                return {false, {}, "CREATE_ITEM_FAILED", "Failed to create item"};
            }
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "rename_item";
        def.displayName = "Rename Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"path", "string"}, {"guid", "string"}, {"object_id", "uint32"}, {"new_name", "string"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString path = args["guid"].toString(args["path"].toString());
            uint32_t objectID = static_cast<uint32_t>(args["object_id"].toInteger(
                                    static_cast<qint64>(args["object_id"].toDouble(0))));
            QString newName = args["new_name"].toString();

            if ((path.isEmpty() && objectID == 0) || newName.isEmpty())
                return {false, {}, "INVALID_ARGS", "path/object_id and new_name are required"};

            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            ProjectItem* item = resolveItemByPathOrObjectID(project, path, objectID);
            if (objectID == 0) {
                const auto selected = resolveProjectSelector(project, path);
                if (!selected.ok)
                    return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
                item = selected.item;
            }
            if (!item)
                return {false, {}, "ITEM_NOT_FOUND",
                        QString("Item '%1' (id=%2) not found").arg(path).arg(objectID)};

            NameValidator::Result vr;
            if (!NameValidator::validateItemName(newName, item->type, &vr))
                return {false, {}, "INVALID_NAME", NameValidator::resultMessage(vr)};

            QString oldName = item->name;
            if (!projectManager->renameItem(item, newName))
                return {false, {}, "RENAME_FAILED",
                        QString("Failed to rename '%1' to '%2'").arg(oldName, newName)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success", true},
                {"old_name", oldName},
                {"new_name", item->name}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "delete_item";
        def.displayName = "Delete Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"path", "string"}, {"object_id", "uint32"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString path = args["path"].toString();
            uint32_t objectID = static_cast<uint32_t>(args["object_id"].toInteger(0));

            if (path.isEmpty() && objectID == 0)
                return {false, {}, "INVALID_ARGS", "path or object_id is required"};

            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            if (!path.isEmpty() && objectID != 0)
            {
                ProjectItem* byPath = findItemByPathInProject(project, path);
                ProjectItem* byID = findItemByObjectIDInProject(project, objectID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS", "path and object_id refer to different items"};
            }

            ProjectItem* item = resolveItemByPathOrObjectID(project, path, objectID);
            if (!path.isEmpty()) {
                const auto selected = resolveProjectSelector(project, path);
                if (!selected.ok)
                    return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
                item = selected.item;
            }

            if (!item)
            {
                return {false, {}, "ITEM_NOT_FOUND",
                        objectID != 0
                            ? QString("Item with object_id '%1' not found").arg(objectID)
                            : QString("Item '%1' not found").arg(path)};
            }


            QString attenuationReferenceError;
            ProjectManager::isReferencedAttenuation(item, nullptr, &attenuationReferenceError);
            if (!attenuationReferenceError.isEmpty())
                return {false, {}, "INVALID_PROJECT", attenuationReferenceError};

            const QString deletedPath = buildItemFullPath(project, item);
            const qint64 deletedID = static_cast<qint64>(itemShortID(item));
            const qint64 deletedHandle = static_cast<qint64>(item->objectID);
            if (!projectManager->deleteItem(item)) {
                ActionResult failed;
                failed.errorCode = QStringLiteral("DELETE_FAILED");
                failed.errorMessage = QStringLiteral("The item could not be deleted");
                return failed;
            }

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success", true},
                {"deleted_object_id", deletedID},
                {"deleted_authoring_handle", deletedHandle},
                {"deleted_path", deletedPath}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    auto selectHandler = [projectManager, projectExplorer, hierarchyResolver](const QJsonObject& args) -> ActionResult {
        if (!projectManager)
            return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
        if (!projectManager->isProjectOpen())
            return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
        Hierarchy* hierarchy = hierarchyResolver ? hierarchyResolver() : nullptr;
        if (!projectExplorer && !hierarchy)
            return {false, {}, "PROJECT_EXPLORER_NOT_AVAILABLE", "No project view is available"};

        QString path = args["path"].toString().trimmed();
        if (path.isEmpty())
            return {false, {}, "INVALID_ARGS", "path is required"};

        ProjectData* project = projectManager->currentProject();
        if (!project)
            return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

        ProjectItem* item = findItemByPathInProject(project, path);
        if (!item)
            return {false, {}, "ITEM_NOT_FOUND",
                    QString("Item '%1' not found").arg(path)};

        if (projectExplorer) Q_ASSERT(QThread::currentThread() == projectExplorer->thread());
        else                 Q_ASSERT(QThread::currentThread() == hierarchy->thread());

        bool peOk = projectExplorer && projectExplorer->selectProjectItem(item);
        bool hOk  = hierarchy && hierarchy->selectProjectItem(item);
        if (!peOk && !hOk)
        {
            return {false, {}, "ITEM_NOT_VISIBLE",
                    QString("Item '%1' exists but is not visible in any project view").arg(path)};
        }

        ActionResult r;
        r.success = true;
        r.data = QJsonObject{
            {"path", path},
            {"name", item->name},
            {"type", itemTypeToString(item->type)},
            {"selected", true}
        };
        return r;
    };

    {
        ActionDef def;
        def.id = "select_project_item";
        def.displayName = "Select Project Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"path", "string"}};
        def.handler = selectHandler;
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "select_item";
        def.displayName = "Select Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"path", "string"}};
        def.handler = selectHandler;
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "move_item";
        def.displayName = "Move Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"path", "string"}, {"object_id", "uint32"}, {"new_parent", "string"}, {"new_parent_id", "uint32"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            QString path = args["path"].toString().trimmed();
            uint32_t objectID = static_cast<uint32_t>(args["object_id"].toDouble(0));
            QString newParentPath = args["new_parent"].toString().trimmed();
            uint32_t newParentID = static_cast<uint32_t>(args["new_parent_id"].toDouble(0));

            if (path.isEmpty() && objectID == 0)
                return {false, {}, "INVALID_ARGS", "path or object_id is required"};
            if (newParentPath.isEmpty() && newParentID == 0)
                return {false, {}, "INVALID_ARGS", "new_parent or new_parent_id is required"};

            ProjectItem* item = resolveItemByPathOrObjectID(project, path, objectID);
            if (!item)
                return {false, {}, "ITEM_NOT_FOUND",
                        QString("Item '%1' (id=%2) not found").arg(path).arg(objectID)};

            ProjectItem* newParent = resolveItemByPathOrObjectID(project, newParentPath, newParentID);
            if (!newParent)
                return {false, {}, "PARENT_NOT_FOUND",
                        QString("New parent '%1' (id=%2) not found").arg(newParentPath).arg(newParentID)};

            bool success = projectManager->moveItem(item, newParent);
            if (!success)
                return {false, {}, "MOVE_FAILED",
                        QString("Failed to move '%1' to '%2'").arg(item->name, newParent->name)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"name", item->name},
                {"object_id", static_cast<qint64>(itemShortID(item))},
                {"authoring_handle", static_cast<qint64>(item->objectID)},
                {"new_parent", newParent->name},
                {"new_parent_id", static_cast<qint64>(itemShortID(newParent))},
                {"new_parent_authoring_handle", static_cast<qint64>(newParent->objectID)}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    for (const auto& kind : { std::make_pair(QString("copy_item"), ProjectManager::ClipboardOp::Copy),
                              std::make_pair(QString("cut_item"),  ProjectManager::ClipboardOp::Cut) })
    {
        ActionDef def;
        def.id = kind.first;
        def.displayName = (kind.second == ProjectManager::ClipboardOp::Copy) ? "Copy Item" : "Cut Item";
        def.category = "project";
        def.undoable = false;
        const ProjectManager::ClipboardOp op = kind.second;
        def.handler = [projectManager, op](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            QString path = args["path"].toString().trimmed();
            uint32_t objectID = static_cast<uint32_t>(args["object_id"].toDouble(0));
            if (path.isEmpty() && objectID == 0)
                return {false, {}, "INVALID_ARGS", "path or object_id is required"};

            ProjectItem* item = resolveItemByPathOrObjectID(project, path, objectID);
            if (!item)
                return {false, {}, "ITEM_NOT_FOUND",
                        QString("Item '%1' (id=%2) not found").arg(path).arg(objectID)};

            const int stored = projectManager->setClipboard(QList<ProjectItem*>{ item }, op);
            if (stored == 0)
                return {false, {}, "NOT_ELIGIBLE",
                        QString("'%1' cannot be placed on the clipboard (folders and buses are excluded).").arg(item->name)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"stored", stored},
                {"op", (op == ProjectManager::ClipboardOp::Copy) ? "copy" : "cut"},
                {"name", item->name},
                {"object_id", static_cast<qint64>(itemShortID(item))},
                {"type", itemTypeToString(item->type)}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "paste_item";
        def.displayName = "Paste Item";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"target_path", "string"}, {"target_object_id", "uint32"}, {"source_path", "string"}, {"source_object_id", "uint32"}, {"op", "string"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            QString targetPath = args["target_path"].toString().trimmed();
            uint32_t targetID = static_cast<uint32_t>(args["target_object_id"].toDouble(0));
            if (targetPath.isEmpty() && targetID == 0)
                return {false, {}, "INVALID_ARGS", "target_path or target_object_id is required"};

            ProjectItem* target = resolveItemByPathOrObjectID(project, targetPath, targetID);
            if (!target)
                return {false, {}, "PARENT_NOT_FOUND",
                        QString("Target '%1' (id=%2) not found").arg(targetPath).arg(targetID)};

            QString srcPath = args["source_path"].toString().trimmed();
            uint32_t srcID = static_cast<uint32_t>(args["source_object_id"].toDouble(0));
            if (!srcPath.isEmpty() || srcID != 0) {
                ProjectItem* src = resolveItemByPathOrObjectID(project, srcPath, srcID);
                if (!src)
                    return {false, {}, "SOURCE_NOT_FOUND",
                            QString("Source '%1' (id=%2) not found").arg(srcPath).arg(srcID)};
                const QString opStr = args["op"].toString().trimmed().toLower();
                const ProjectManager::ClipboardOp op = (opStr == "cut")
                    ? ProjectManager::ClipboardOp::Cut : ProjectManager::ClipboardOp::Copy;
                if (projectManager->setClipboard(QList<ProjectItem*>{ src }, op) == 0)
                    return {false, {}, "NOT_ELIGIBLE",
                            QString("'%1' cannot be placed on the clipboard.").arg(src->name)};
            }

            std::vector<ProjectItem*> created = projectManager->pasteClipboard(target);
            if (created.empty())
                return {false, {}, "PASTE_FAILED", "Nothing was pasted (empty clipboard or incompatible target)"};

            QJsonArray items;
            for (ProjectItem* it : created) {
                items.append(QJsonObject{
                    {"name", it->name},
                    {"object_id", static_cast<qint64>(itemShortID(it))},
                    {"authoring_handle", static_cast<qint64>(it->objectID)},
                    {"guid", it->guid.toString(QUuid::WithoutBraces)},
                    {"path", buildItemFullPath(project, it)}
                });
            }
            ActionResult r;
            r.success = true;
            r.data = QJsonObject{ {"pasted", static_cast<int>(created.size())}, {"items", items} };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "paste_properties";
        def.displayName = "Paste Properties";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"source_path", "string"}, {"source_object_id", "uint32"}, {"target_path", "string"}, {"target_object_id", "uint32"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager)
                return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
            if (!projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            ProjectData* project = projectManager->currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            QString srcPath = args["source_path"].toString().trimmed();
            uint32_t srcID = static_cast<uint32_t>(args["source_object_id"].toDouble(0));
            QString tgtPath = args["target_path"].toString().trimmed();
            uint32_t tgtID = static_cast<uint32_t>(args["target_object_id"].toDouble(0));
            if (srcPath.isEmpty() && srcID == 0)
                return {false, {}, "INVALID_ARGS", "source_path or source_object_id is required"};
            if (tgtPath.isEmpty() && tgtID == 0)
                return {false, {}, "INVALID_ARGS", "target_path or target_object_id is required"};

            ProjectItem* source = resolveItemByPathOrObjectID(project, srcPath, srcID);
            if (!source)
                return {false, {}, "SOURCE_NOT_FOUND",
                        QString("Source '%1' (id=%2) not found").arg(srcPath).arg(srcID)};
            ProjectItem* target = resolveItemByPathOrObjectID(project, tgtPath, tgtID);
            if (!target)
                return {false, {}, "ITEM_NOT_FOUND",
                        QString("Target '%1' (id=%2) not found").arg(tgtPath).arg(tgtID)};

            if (!projectManager->pasteProperties(source, QList<ProjectItem*>{ target }))
                return {false, {}, "PASTE_PROPERTIES_FAILED",
                        QString("Could not paste properties from '%1' onto '%2' (type mismatch or nothing copyable)").arg(source->name, target->name)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"source", source->name},
                {"target", target->name},
                {"target_object_id", static_cast<qint64>(itemShortID(target))}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_item_property";
        def.displayName = "Set Item Property";
        def.category = "project";
        def.paramSchema = setItemPropertySchema();
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            ProjectData* project = projectManager->currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            QString path = args["path"].toString().trimmed();
            uint32_t objectID = static_cast<uint32_t>(args["object_id"].toInteger(
                                    static_cast<qint64>(args["object_id"].toDouble(0))));
            uint32_t authoringHandle = static_cast<uint32_t>(args["authoring_handle"].toInteger(
                                    static_cast<qint64>(args["authoring_handle"].toDouble(0))));
            QString propertyName = args["property"].toString().trimmed();

            if ((path.isEmpty() && objectID == 0 && authoringHandle == 0) || propertyName.isEmpty())
                return {false, {}, "INVALID_ARGS",
                        "path/object_id/authoring_handle and property are required"};
            const QString overrideOperation = args.value(
                QStringLiteral("override_operation")).toString();
            const bool clearOverride =
                overrideOperation == QLatin1String("clear_override")
                || overrideOperation == QLatin1String("clear_platform_override");
            if (!args.contains(QStringLiteral("value")) && !clearOverride)
                return {false, {}, "INVALID_ARGS", "value is required"};

            ProjectItem* item = nullptr;
            if (authoringHandle != 0)
            {
                item = findItemByAuthoringHandleInProject(project, authoringHandle);
                if (!item)
                    item = projectManager->findBusItem(authoringHandle);
                if (!item)
                    return {false, {}, "ITEM_NOT_FOUND",
                            QString("Item with authoring_handle=%1 not found").arg(authoringHandle)};
            }
            if (!item && !path.isEmpty()) {
                const auto selected = resolveProjectSelector(project, path);
                if (!selected.ok)
                    return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
                item = selected.item;
            }
            if (!item)
                item = resolveItemByPathOrObjectID(project, path, objectID);
            if (!item)
                return {false, {}, "ITEM_NOT_FOUND",
                        QString("Item '%1' (id=%2) not found").arg(path).arg(objectID)};
            if (claimsFieldGroupItemProperty(*item, propertyName))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Main Output, Send, or Spatial Rating"};
            if (clearOverride && !claimsPlayableItemProperty(item, propertyName)) {
                const bool busOutputVolume =
                    item->comp<VBus>() != nullptr
                    && propertyName == QStringLiteral("outputBusVolume");
                if (!busOutputVolume)
                    return {false, {}, "INVALID_ARGS",
                            "Clear Override is only valid for generated playable properties"};
            }

            for (const auto& route : kItemPropertyRoutes) {
                if (route.claims(item, propertyName))
                    return route.handle(projectManager, item, propertyName, args,
                                        ActionManager::instance().executeOriginator());
            }
            return {false, {}, "UNKNOWN_PROPERTY",
                    QString("Unknown property '%1'").arg(propertyName)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_pack_export_current_platform";
        def.displayName = "Set Pack Platform";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"platform_id", "string"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            if (args.contains("platform") || !args.value("platform_id").isString())
                return {false, {}, "INVALID_ARGS", "Use the 'platform_id' string argument"};

            const QString newValue = args.value("platform_id").toString();
            QString platformError;
            if (!ProjectPlatformSemantics::validateIds({newValue}, projectManager->currentProject()->platforms, &platformError))
                return {false, {}, "INVALID_ARGS", platformError};
            const QString oldValue = projectManager->getPackExportCurrentPlatform();

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"platform_id", newValue}};

            if (newValue == oldValue) {
                return r;
            }

            projectManager->setPackExportCurrentPlatformInternal(newValue);

            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_pack_export_selected_platforms";
        def.displayName = "Set Pack Platforms";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"platform_ids", "array"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            if (args.contains("platforms") || !args.value("platform_ids").isArray())
                return {false, {}, "INVALID_ARGS", "Use the 'platform_ids' array argument"};

            QStringList newValue;
            for (const auto& v : args.value("platform_ids").toArray())
                newValue.append(v.toString());
            QString platformError;
            if (!ProjectPlatformSemantics::validateIds(newValue, projectManager->currentProject()->platforms, &platformError))
                return {false, {}, "INVALID_ARGS", platformError};

            const QStringList oldValue = projectManager->getPackExportSelectedPlatforms();

            ActionResult r;
            r.success = true;
            QJsonArray echo;
            for (const auto& p : newValue) echo.append(p);
            r.data = QJsonObject{{"platform_ids", echo}};

            if (newValue == oldValue) {
                return r;
            }

            projectManager->setPackExportSelectedPlatformsInternal(newValue);

            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_target_platforms";
        def.displayName = "Set Target Platforms";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"platforms", "array"}};
        def.handler = [](const QJsonObject&) -> ActionResult {
            return {false, {}, "UNSUPPORTED_PLATFORM_FORMAT",
                    "Base platform lists are no longer supported"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_project_languages";
        def.displayName = "Set Target Languages";
        def.category = "project";
        def.undoable = true;
        def.paramSchema = QJsonObject{{"languages", "array"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            if (!args.contains("languages") || !args.value("languages").isArray())
                return {false, {}, "INVALID_ARGS", "'languages' array argument is required"};

            QStringList requested;
            for (const auto& v : args.value("languages").toArray())
                requested.append(v.toString());

            const auto normalized = ProjectLanguageSemantics::normalize(requested);
            if (!normalized.ok())
                return {false, {}, "INVALID_ARGS", normalized.error};

            const QStringList newValue = normalized.value;
            const QStringList oldValue = projectManager->getProjectLanguages();

            ActionResult r;
            r.success = true;
            QJsonArray echo;
            for (const auto& lang : newValue) echo.append(lang);
            r.data = QJsonObject{{"languages", echo}};

            if (newValue == oldValue) {
                return r;
            }

            projectManager->setProjectLanguagesInternal(newValue);

            r.undoFn = [oldValue]() {
                ProjectManager::instance().setProjectLanguagesInternal(oldValue);
            };
            r.redoFn = [newValue]() {
                ProjectManager::instance().setProjectLanguagesInternal(newValue);
            };
            r.undoText = QStringLiteral("Set Target Languages");
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_pack_build_output_dir";
        def.displayName = "Set Pack Build Output Directory";
        def.category = "project";
        def.undoable = true;
        def.paramSchema = QJsonObject{{"path", "string"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            const QJsonValue path = args.value("path");
            if (!path.isString() || !PathUtils::isValidPackBuildOutputDir(path.toString(), projectManager->currentProject()->projectPath))
                return {false, {}, "INVALID_ARGS",
                        QStringLiteral("Pack output must use a relative directory path. ")
                            + PathUtils::packBuildOutputLocationError()};
            const QString newValue = PathUtils::normalizePackBuildOutputDir(path.toString());
            const QString oldValue = projectManager->packBuildOutputDir();

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"path", newValue}};

            if (newValue == oldValue) {
                return r;
            }

            projectManager->setPackBuildOutputDir(newValue);

            r.undoFn = [oldValue]() {
                ProjectManager::instance().setPackBuildOutputDirInternal(oldValue);
            };
            r.redoFn = [newValue]() {
                ProjectManager::instance().setPackBuildOutputDir(newValue);
            };
            r.undoText = QStringLiteral("Set Pack Build Output Directory");
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_action_pack_targeting";
        def.displayName = "Set Action Pack Targeting";
        def.category = "project";
        def.undoable = false;
        def.paramSchema = QJsonObject{{"stableId", "string"}, {"allPlatforms", "bool?"},
                                      {"platform_ids", "array?"}, {"language", "string?"}};
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            if (args.contains("platforms")
                || (args.contains("platform_ids") && !args.value("platform_ids").isArray()))
                return {false, {}, "INVALID_ARGS", "Use the 'platform_ids' array argument"};

            harkenstudio::ObjectId stableId{};
            if (!parseObjectIdJson(args.value("stableId"), &stableId) || harkenstudio::IsNull(stableId))
                return {false, {}, "INVALID_ARGS", "'stableId' argument is required"};

            const std::set<harkenstudio::ObjectId> validIds =
                ProjectPackExporter::CollectActionStableIds(*projectManager->currentProject());
            if (!validIds.count(stableId))
                return {false, {}, "ACTION_NOT_FOUND",
                        QStringLiteral("Stable id %1 does not resolve to an Action")
                            .arg(objectIdString(stableId))};

            QStringList platforms;
            for (const auto& v : args.value("platform_ids").toArray())
                platforms.append(v.toString());
            QString platformError;
            if (!ProjectPlatformSemantics::validateIds(platforms, projectManager->currentProject()->platforms, &platformError))
                return {false, {}, "INVALID_ARGS", platformError};

            const bool allPlatforms = args.contains("allPlatforms")
                ? args.value("allPlatforms").toBool()
                : platforms.isEmpty();

            std::optional<QString> language;
            if (args.contains("language")) {
                if (!args.value("language").isString())
                    return {false, {}, "INVALID_ARGS", "'language' must be a string"};
                language = args.value("language").toString();
                if (!language->isEmpty() && !ProjectLanguageSemantics::buildLanguages(
                        *projectManager->currentProject()).contains(*language))
                    return {false, {}, "INVALID_ARGS", "Unknown project language"};
            }

            std::vector<ActionPackTargeting> before = projectManager->getActionPackTargeting();
            std::vector<ActionPackTargeting> working = before;
            const ActionPackTargeting updated{stableId, allPlatforms,
                allPlatforms ? QStringList{} : platforms, language};
            if (language) {
                auto entry = std::find_if(working.begin(), working.end(), [&](const auto& value) {
                    return value.stableId == stableId && value.language == language;
                });
                if (entry == working.end()) working.push_back(updated);
                else *entry = updated;
            } else {
                working.erase(std::remove_if(working.begin(), working.end(),
                    [&](const auto& entry) { return entry.stableId == stableId; }), working.end());
                if (!allPlatforms) working.push_back(updated);
            }

            ActionResult r;
            r.success = true;
            QJsonArray echo;
            for (const auto& p : platforms) echo.append(p);
            r.data = QJsonObject{{"stableId", objectIdString(stableId)},
                                 {"allPlatforms", allPlatforms},
                                 {"platform_ids", echo}};

            if (language) r.data.insert("language", *language);

            if (working == before)
                return r;

            projectManager->setActionPackTargetingInternal(std::move(working));
            return r;
        };
        mgr.registerAction(def);
    }

}

}
