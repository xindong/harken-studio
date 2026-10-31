#include "Tooling/Harkenizer/Core/PackBuildPlan.h"

#include "Components/VAction.h"
#include "Components/VSound.h"
#include "NameValidator.h"
#include "ProjectLanguageSemantics.h"
#include "RuntimeCapabilityMatrix.h"
#include "BindingRuntimeLowering.h"

#include <QJsonArray>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace VNX {

const BindingRuntimeLowering& PackBuildContext::bindingLowering()
{
    if (!bindings_)
        bindings_ = std::make_shared<BindingRuntimeLowering>(BindingRuntimeLoweringSemantics::lowerProject(*project));
    return *bindings_;
}

bool PackBuildContext::validatePlayback(QString* error, PlaybackReferenceError* code)
{
    if (!playbackValid_)
        playbackValid_ = validatePlaybackReferences(*project, &playbackError_, &playbackErrorCode_);
    if (!*playbackValid_) {
        if (error) *error = playbackError_;
        if (code) *code = playbackErrorCode_;
    }
    return *playbackValid_;
}

namespace PackBuildPlanning {
namespace {

std::vector<const ProjectItem*> actions(const ProjectData& project,
                                        harkenstudio::ObjectId actionStableId)
{
    std::vector<const ProjectItem*> result;
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        std::function<void(const ProjectItem*)> visit = [&](const ProjectItem* item) {
            if (!item) return;
            if (item->type == ProjectItem::Type::Action && !harkenstudio::IsNull(item->runtimeId())
                && (harkenstudio::IsNull(actionStableId) || item->runtimeId() == actionStableId))
                result.push_back(item);
            for (const auto& child : item->children) visit(child.get());
        };
        visit(binding->rootItem.get());
    }
    std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) {
        return a->runtimeId() < b->runtimeId();
    });
    return result;
}

QStringList requestedOrProjectLanguages(const ProjectData& project,
                                        const QStringList& requestedLanguages,
                                        QString* error)
{
    const QStringList projectLanguages = ProjectLanguageSemantics::buildLanguages(project);
    if (requestedLanguages.isEmpty())
        return projectLanguages;
    QStringList languages;
    for (const QString& raw : requestedLanguages) {
        const QString language = raw.trimmed();
        if (!projectLanguages.contains(language)) {
            *error = QStringLiteral("Requested language '%1' is not a project language")
                         .arg(language);
            return {};
        }
        if (!languages.contains(language))
            languages.append(language);
    }
    return languages;
}

bool localizedClosure(const ActionClosurePlan& closure, const ActionClosureBuildContext& context)
{
    const auto localized = [&](const auto& members) {
        return std::any_of(members.begin(), members.end(), [&](const auto& member) {
            if (member.type != ProjectItem::Type::Sound) return false;
            const ProjectItem* item = context.findObject(member.objectId);
            const auto* sound = item ? item->comp<VSound>() : nullptr;
            return sound && sound->distinguishLanguage;
        });
    };
    return localized(closure.objects) || localized(closure.controlObjects);
}

std::map<harkenstudio::ObjectId, bool> localizedActionsFor(
    const std::vector<const ProjectItem*>& actionItems,
    const ActionClosureBuildContext& context)
{
    std::map<harkenstudio::ObjectId, bool> localizedActions;
    for (const auto* action : actionItems) {
        const auto closure = context.contentReferences(action->runtimeId());
        localizedActions.emplace(action->runtimeId(), localizedClosure(closure, context));
    }
    return localizedActions;
}

PackBuildPlan buildImpl(const ProjectData& project, harkenstudio::ObjectId actionStableId,
                        const QStringList& requestedPlatforms,
                        const QStringList& requestedLanguages,
                        const QString& singleActionArtifactName, bool includeUnchecked,
                        const PackExportSelection& selection,
                        const RuntimeCapabilityContext& context,
                        const ActionClosureBuildContext* closures)
{
    PackBuildPlan plan;
    const QStringList platforms = requestedPlatforms.isEmpty()
        ? project.platformIds() : requestedPlatforms;
    QString platformError;
    if (!ProjectPlatformSemantics::validatePlatforms(project.platforms, &platformError)
        || !ProjectPlatformSemantics::validateIds(platforms, project.platforms, &platformError)) {
        plan.diagnostics.push_back({ActionClosureDiagnosticCode::InvalidBuildPlatform,
            platformError, actionStableId, -1, {}, QStringLiteral("platformIds"), {}});
        return plan;
    }
    QString languageError;
    const QStringList languages =
        requestedOrProjectLanguages(project, requestedLanguages, &languageError);
    std::unique_ptr<ActionClosureBuildContext> ownedClosures;
    if (!closures) {
        ownedClosures = std::make_unique<ActionClosureBuildContext>(project);
        closures = ownedClosures.get();
    }
    const auto actionItems = actions(project, actionStableId);
    const auto localization = localizedActionsFor(actionItems, *closures);
    for (const auto* action : actionItems) {
        const QString artifactName = singleActionArtifactName.isEmpty()
            ? action->name : singleActionArtifactName;
        const bool localized = localization.at(action->runtimeId());
        const QStringList variantLanguages = localized ? languages : QStringList{QString()};
        if (!includeUnchecked && std::none_of(platforms.begin(), platforms.end(), [&](const auto& platform) {
                if (variantLanguages.isEmpty())
                    return selection.includesActionPack(action->runtimeId(), platform, {});
                return std::any_of(variantLanguages.begin(), variantLanguages.end(), [&](const auto& language) {
                    return selection.includesActionPack(action->runtimeId(), platform, language);
                });
            }))
            continue;
        if (localized) {
            if (!languageError.isEmpty()) {
                plan.diagnostics.push_back({
                    ActionClosureDiagnosticCode::InvalidBuildLanguage,
                    languageError, action->runtimeId(), -1, action->runtimeId(),
                    QStringLiteral("languages"), {}});
                continue;
            }
            if (variantLanguages.isEmpty()) {
                plan.diagnostics.push_back({
                    ActionClosureDiagnosticCode::LocalizedWithoutLanguages,
                    QStringLiteral(
                        "Pack containing Action %1 requires language variants but the project has no languages")
                        .arg(objectIdString(action->runtimeId())),
                    action->runtimeId(), -1, action->runtimeId(),
                    QStringLiteral("distinguishLanguage"), {}});
                continue;
            }
        }
        const QString nameError = packNameError(artifactName);
        if (!nameError.isEmpty()) {
            plan.diagnostics.push_back({ActionClosureDiagnosticCode::InvalidPackName,
                nameError,
                action->runtimeId(), -1, action->runtimeId(), QStringLiteral("name"), {}});
            if (!includeUnchecked) continue;
        }
        for (const QString& platform : platforms) {
            const auto& definition = *ProjectPlatformSemantics::find(project.platforms, QUuid::fromString(platform));
            for (const QString& language : variantLanguages) {
                if (!includeUnchecked && !selection.includesActionPack(action->runtimeId(), platform, language))
                    continue;
                PackBuildVariant variant;
                variant.actionStableId = action->runtimeId();
                variant.variant = {platform, language};
                variant.platformDefinition = definition;
                variant.artifactName = artifactName;
                variant.relativePackPath =
                    relativePackPath(definition.name, language, variant.artifactName);
                variant.closure = ActionClosureSemantics::buildForContext(
                    project, action->runtimeId(), variant.variant, context, closures);
                variant.capability = RuntimeCapabilityMatrix::assessForContext(
                    *action, project, RuntimeCapabilityOperation::PackBuild, variant.variant, context);
                std::map<harkenstudio::ObjectId, std::vector<harkenstudio::ObjectId>> dependencies;
                for (const auto& edge : variant.closure.edges)
                    if (edge.reason == ActionClosureEdgeReason::PostAction)
                        dependencies[edge.fromStableId].push_back(edge.toStableId);
                for (auto& [source, targets] : dependencies) {
                    std::sort(targets.begin(), targets.end());
                    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                }
                std::set<harkenstudio::ObjectId> visited;
                std::set<harkenstudio::ObjectId> active;
                std::function<void(harkenstudio::ObjectId)> append = [&](harkenstudio::ObjectId id) {
                    if (visited.count(id) != 0 || active.count(id) != 0) return;
                    active.insert(id);
                    for (harkenstudio::ObjectId dependency : dependencies[id]) append(dependency);
                    active.erase(id);
                    visited.insert(id);
                    variant.dependencyOrder.push_back(id);
                };
                append(action->runtimeId());
                plan.diagnostics.insert(plan.diagnostics.end(), variant.closure.diagnostics.begin(),
                                        variant.closure.diagnostics.end());
                plan.variants.push_back(std::move(variant));
            }
        }
    }
    for (const auto& variant : plan.variants) {
        PackBuildUnit unit;
        unit.name = variant.artifactName;
        unit.artifactKey = stableArtifactKey(variant.actionStableId);
        unit.variant = variant.variant;
        unit.platformDefinition = variant.platformDefinition;
        unit.relativePackPath = variant.relativePackPath;
        unit.actionStableIds = {variant.actionStableId};
        plan.units.push_back(std::move(unit));
    }
    std::map<harkenstudio::ObjectId, int> references;
    std::map<QString, int> mediaReferences;
    for (const auto& unit : plan.units) {
        std::set<harkenstudio::ObjectId> unitActions;
        std::set<QString> unitMedia;
        for (const auto& variant : plan.variants) {
            if (variant.variant.platform != unit.variant.platform
                || variant.variant.language != unit.variant.language
                || std::find(unit.actionStableIds.begin(), unit.actionStableIds.end(),
                             variant.actionStableId) == unit.actionStableIds.end())
                continue;
            for (const auto& action : variant.closure.actions)
                unitActions.insert(action.stableId);
            for (const auto& media : variant.closure.media)
                unitMedia.insert(media.artifactId);
        }
        for (const auto id : unitActions) ++references[id];
        for (const auto& id : unitMedia) ++mediaReferences[id];
    }
    for (const auto& [id, count] : references)
        if (count > 1) plan.sharedObjectIds.insert(id);
    for (const auto& [id, count] : mediaReferences)
        if (count > 1) plan.sharedArtifactIds.insert(id);
    plan.groupMetadata = QJsonObject{{QStringLiteral("groups"), QJsonArray{}}};
    return plan;
}

bool localizedAction(const ProjectData& project, harkenstudio::ObjectId actionStableId)
{
    const ActionClosureBuildContext context(project);
    for (const auto* action : actions(project, actionStableId)) {
        if (localizedClosure(context.contentReferences(action->runtimeId()), context))
            return true;
    }
    return false;
}

}

QHash<harkenstudio::ObjectId, QStringList> artifactLanguages(
    const ProjectData& project, const QStringList& requestedPlatforms)
{
    QHash<harkenstudio::ObjectId, QStringList> result;
    const QStringList platforms = requestedPlatforms.isEmpty()
        ? project.platformIds() : requestedPlatforms;
    if (!ProjectPlatformSemantics::validatePlatforms(project.platforms)
        || !ProjectPlatformSemantics::validateIds(platforms, project.platforms))
        return result;

    return actionLanguages(project);
}

QString stableArtifactKey(harkenstudio::ObjectId actionStableId)
{
    return QStringLiteral("action_%1").arg(objectIdString(actionStableId));
}

QString packNameError(const QString& name, bool grouped)
{
    QString reason;
    NameValidator::Result result;
    if (name.compare(QStringLiteral("Init"), Qt::CaseInsensitive) == 0)
        reason = QStringLiteral("Init is reserved for the system Pack.");
    else if (!NameValidator::validateFileName(name, &result))
        reason = NameValidator::resultMessage(result);
    return reason.isEmpty() ? QString()
        : QStringLiteral("%1 '%2' cannot be exported: %3")
              .arg(grouped ? QStringLiteral("Pack group") : QStringLiteral("Action"), name, reason);
}

QString relativePackPath(const QString& platform, const QString& language,
                         const QString& artifact)
{
    QString path = platform;
    if (!language.trimmed().isEmpty())
        path += QLatin1Char('/') + language.trimmed();
    path += QLatin1Char('/') + artifact;
    return path;
}

QString packFileName(const QString& language, const QString& artifact,
                     const QString& extension)
{
    if (language.trimmed().isEmpty())
        return artifact + extension;
    return language.trimmed() + QLatin1Char('/') + artifact + extension;
}

bool actionIsLocalized(const ProjectData& project, harkenstudio::ObjectId actionStableId)
{
    return localizedAction(project, actionStableId);
}

QHash<harkenstudio::ObjectId, QStringList> actionLanguages(const ProjectData& project)
{
    const auto actionItems = actions(project, {});
    const ActionClosureBuildContext context(project);
    const auto localization = localizedActionsFor(actionItems, context);
    const auto languages = ProjectLanguageSemantics::buildLanguages(project);
    QHash<harkenstudio::ObjectId, QStringList> result;
    for (const auto* action : actionItems)
        result.insert(action->runtimeId(), localization.at(action->runtimeId())
            ? languages : QStringList{QString()});
    return result;
}

PackBuildPlan build(const ProjectData& project, const QStringList& requestedPlatforms,
                    const QStringList& requestedLanguages, bool includeUnchecked,
                    const PackExportSelection& selection, const RuntimeCapabilityContext& context,
                    const ActionClosureBuildContext* closures)
{
    return buildImpl(project, {}, requestedPlatforms, requestedLanguages, {}, includeUnchecked, selection, context, closures);
}

PackBuildPlan buildForAction(const ProjectData& project, harkenstudio::ObjectId actionStableId,
                            const QStringList& requestedPlatforms,
                            const QStringList& requestedLanguages,
                            const QString& artifactName, bool includeUnchecked,
                            const PackExportSelection& selection, const RuntimeCapabilityContext& context,
                            const ActionClosureBuildContext* closures)
{
    return buildImpl(project, actionStableId, requestedPlatforms, requestedLanguages, artifactName, includeUnchecked, selection, context, closures);
}

}

bool PackBuildPlan::ready() const
{
    return diagnostics.empty() && std::all_of(variants.begin(), variants.end(),
        [](const auto& variant) {
            return variant.closure.ready() && variant.capability.permitted();
        });
}

QJsonObject PackBuildPlan::toJson(const ProjectData& project) const
{
    QJsonArray unitsJson;
    for (const auto& unit : units) {
        QJsonArray actions;
        for (const auto id : unit.actionStableIds)
            actions.append(objectIdString(id));
        unitsJson.append(QJsonObject{
            {QStringLiteral("name"), unit.name},
            {QStringLiteral("artifactKey"), unit.artifactKey},
            {QStringLiteral("relativePackPath"), unit.relativePackPath},
            {QStringLiteral("platform_id"), unit.variant.platform},
            {QStringLiteral("language"), unit.variant.language},
            {QStringLiteral("actionStableIds"), actions},
            {QStringLiteral("grouped"), unit.grouped}});
    }
    QJsonArray variantsJson;
    for (const auto& variant : variants)
        variantsJson.append(QJsonObject{
            {QStringLiteral("actionStableId"), objectIdString(variant.actionStableId)},
            {QStringLiteral("artifactName"), variant.artifactName},
            {QStringLiteral("relativePackPath"), variant.relativePackPath},
            {QStringLiteral("platform_id"), variant.variant.platform},
            {QStringLiteral("platform_name"), variant.platformDefinition.name},
            {QStringLiteral("base_platform"), ProjectPlatformSemantics::basePlatformName(variant.platformDefinition.basePlatform)},
            {QStringLiteral("language"), variant.variant.language},
            {QStringLiteral("dependencyOrder"), [&variant] {
                QJsonArray values;
                for (harkenstudio::ObjectId id : variant.dependencyOrder)
                    values.append(objectIdString(id));
                return values;
            }()},
            {QStringLiteral("capability"), variant.capability.toJson()},
            {QStringLiteral("closure"), variant.closure.toJson(project)}});
    QJsonArray objects;
    for (const auto id : sharedObjectIds) objects.append(objectIdString(id));
    QJsonArray media;
    for (const auto& artifact : sharedArtifactIds) media.append(artifact);
    return {{QStringLiteral("variants"), variantsJson},
            {QStringLiteral("units"), unitsJson},
            {QStringLiteral("sharedObjectIds"), objects},
            {QStringLiteral("sharedArtifactIds"), media},
            {QStringLiteral("groupMetadata"), groupMetadata},
            {QStringLiteral("ready"), ready()}};
}

}
