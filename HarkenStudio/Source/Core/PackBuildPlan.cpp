#include "PackBuildPlan.h"

#include "Components/VAction.h"
#include "Components/VSound.h"
#include "NameValidator.h"
#include "ProjectLanguageSemantics.h"
#include "RuntimeCapabilityMatrix.h"

#include <QJsonArray>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace VNX {
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

bool includesPlatform(const ProjectData& project, harkenstudio::ObjectId actionId, const QString& platform)
{
    for (const auto& group : project.packGroups) {
        if (std::find(group.actionIds.begin(), group.actionIds.end(), actionId) != group.actionIds.end())
            return group.includesPlatform(platform);
    }
    for (const auto& targeting : project.actionPackTargeting)
        if (targeting.stableId == actionId) return targeting.includesPlatform(platform);
    return true;
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

struct ItemIndex
{
    std::map<uint32_t, const ProjectItem*> byObject;
};

void indexItems(const ProjectItem* item, ItemIndex& index)
{
    if (!item)
        return;
    if (item->objectID != 0)
        index.byObject.emplace(item->objectID, item);
    for (const auto& child : item->children)
        indexItems(child.get(), index);
}

ItemIndex makeItemIndex(const ProjectData& project)
{
    ItemIndex index;
    indexItems(project.root.get(), index);
    for (const ProjectFileBinding* binding : project.allFileBindings())
        if (binding)
            indexItems(binding->rootItem.get(), index);
    return index;
}

const ProjectItem* byObjectId(const ItemIndex& index, uint32_t objectId)
{
    const auto it = index.byObject.find(objectId);
    return it == index.byObject.end() ? nullptr : it->second;
}

bool localizedClosure(const ActionClosurePlan& closure, const ItemIndex& index)
{
    for (const ActionClosureMember& member : closure.objects) {
        if (member.type != ProjectItem::Type::Sound)
            continue;
        const ProjectItem* item = byObjectId(index, member.objectId);
        const auto* sound = item ? item->comp<VSound>() : nullptr;
        if (sound && sound->distinguishLanguage)
            return true;
    }
    return false;
}

PackBuildPlan buildImpl(const ProjectData& project, harkenstudio::ObjectId actionStableId,
                        const QStringList& requestedPlatforms,
                        const QStringList& requestedLanguages,
                        const QString& singleActionArtifactName)
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
    const ItemIndex index = makeItemIndex(project);
    const auto actionItems = actions(project, actionStableId);
    std::map<harkenstudio::ObjectId, const PackGroup*> groupsByAction;
    if (harkenstudio::IsNull(actionStableId)) {
        for (const auto& group : project.packGroups)
            for (const auto member : group.actionIds)
                groupsByAction.emplace(member, &group);
    }
    std::map<harkenstudio::ObjectId, ActionClosurePlan> closures;
    std::map<harkenstudio::ObjectId, bool> localizedActions;
    for (const auto* action : actionItems) {
        auto closure = ActionClosureSemantics::build(project, action->runtimeId());
        localizedActions.emplace(action->runtimeId(), localizedClosure(closure, index));
        closures.emplace(action->runtimeId(), std::move(closure));
    }
    bool changed;
    do {
        changed = false;
        for (const auto* action : actionItems) {
            const auto id = action->runtimeId();
            if (localizedActions.at(id))
                continue;
            const auto group = groupsByAction.find(id);
            const bool groupLocalized = group != groupsByAction.end()
                && std::any_of(group->second->actionIds.begin(), group->second->actionIds.end(),
                    [&](const auto member) {
                        const auto found = localizedActions.find(member);
                        return found != localizedActions.end() && found->second;
                    });
            const bool dependencyLocalized = std::any_of(
                closures.at(id).actions.begin(), closures.at(id).actions.end(),
                [&](const auto& member) {
                    const auto found = localizedActions.find(member.stableId);
                    return found != localizedActions.end() && found->second;
                });
            if (groupLocalized || dependencyLocalized) {
                localizedActions.at(id) = true;
                changed = true;
            }
        }
    } while (changed);
    for (const auto* action : actionItems) {
        const auto group = groupsByAction.find(action->runtimeId());
        const bool grouped = group != groupsByAction.end();
        const QString artifactName = grouped ? group->second->name
            : singleActionArtifactName.isEmpty() ? action->name : singleActionArtifactName;
        const QString nameError = packNameError(artifactName, grouped);
        if (!nameError.isEmpty()) {
            plan.diagnostics.push_back({ActionClosureDiagnosticCode::InvalidPackName,
                nameError,
                action->runtimeId(), -1, action->runtimeId(), QStringLiteral("name"), {}});
            continue;
        }
        const bool localized = localizedActions.at(action->runtimeId());
        QStringList variantLanguages;
        if (localized) {
            if (!languageError.isEmpty()) {
                plan.diagnostics.push_back({
                    ActionClosureDiagnosticCode::InvalidBuildLanguage,
                    languageError, action->runtimeId(), -1, action->runtimeId(),
                    QStringLiteral("languages"), {}});
                continue;
            }
            variantLanguages = languages;
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
        } else {
            variantLanguages.append(QString());
        }
        for (const QString& platform : platforms) {
            if (!includesPlatform(project, action->runtimeId(), platform)) continue;
            const auto& definition = *ProjectPlatformSemantics::find(project.platforms, QUuid::fromString(platform));
            for (const QString& language : variantLanguages) {
                PackBuildVariant variant;
                variant.actionStableId = action->runtimeId();
                variant.variant = {platform, language};
                variant.platformDefinition = definition;
                variant.artifactName = artifactName;
                variant.relativePackPath =
                    relativePackPath(definition.name, language, variant.artifactName);
                variant.closure = ActionClosureSemantics::build(
                    project, action->runtimeId(), variant.variant);
                for (const auto& dependency : variant.closure.actions) {
                    if (!includesPlatform(project, dependency.stableId, platform))
                        variant.closure.diagnostics.push_back({
                            ActionClosureDiagnosticCode::ExcludedDependency,
                            QStringLiteral("Action %1 depends on Action %2 excluded from platform '%3'")
                                .arg(objectIdString(action->runtimeId())).arg(objectIdString(dependency.stableId)).arg(definition.name),
                            action->runtimeId(), -1, dependency.stableId,
                            QStringLiteral("platformIds"), dependency.referencePath});
                }
                variant.capability = RuntimeCapabilityMatrix::assess(
                    *action, project, RuntimeCapabilityOperation::PackBuild, variant.variant);
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
    std::map<std::tuple<QString, QString, QString>, std::size_t> unitIndices;
    for (const auto& variant : plan.variants) {
        const auto group = groupsByAction.find(variant.actionStableId);
        const bool grouped = group != groupsByAction.end();
        const QString artifactKey = grouped
            ? QStringLiteral("group_") + group->second->name
            : stableArtifactKey(variant.actionStableId);
        const auto key = std::make_tuple(artifactKey, variant.variant.platform,
                                        variant.variant.language);
        auto [position, inserted] = unitIndices.emplace(key, plan.units.size());
        if (inserted) {
            PackBuildUnit unit;
            unit.name = variant.artifactName;
            unit.artifactKey = artifactKey;
            unit.variant = variant.variant;
            unit.platformDefinition = variant.platformDefinition;
            unit.relativePackPath = variant.relativePackPath;
            unit.grouped = grouped;
            plan.units.push_back(std::move(unit));
        }
        plan.units[position->second].actionStableIds.push_back(variant.actionStableId);
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
    QJsonArray groups;
    for (const auto& group : project.packGroups) {
        QJsonArray ids;
        for (const auto id : group.actionIds)
            ids.append(objectIdString(id));
        QJsonArray platformsJson;
        for (const auto& platform : group.platformIds)
            platformsJson.append(platform);
        groups.append(QJsonObject{{QStringLiteral("name"), group.name},
                                   {QStringLiteral("actionIds"), ids},
                                   {QStringLiteral("allPlatforms"), group.allPlatforms},
                                   {QStringLiteral("platform_ids"), platformsJson}});
    }
    plan.groupMetadata = QJsonObject{{QStringLiteral("groups"), groups}};
    return plan;
}

bool localizedAction(const ProjectData& project, harkenstudio::ObjectId actionStableId)
{
    const ItemIndex index = makeItemIndex(project);
    for (const auto* action : actions(project, actionStableId)) {
        if (localizedClosure(ActionClosureSemantics::build(project, action->runtimeId()), index))
            return true;
    }
    return false;
}

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

PackBuildPlan build(const ProjectData& project, const QStringList& requestedPlatforms,
                    const QStringList& requestedLanguages)
{
    return buildImpl(project, {}, requestedPlatforms, requestedLanguages, {});
}

PackBuildPlan buildForAction(const ProjectData& project, harkenstudio::ObjectId actionStableId,
                            const QStringList& requestedPlatforms,
                            const QStringList& requestedLanguages,
                            const QString& artifactName)
{
    return buildImpl(project, actionStableId, requestedPlatforms, requestedLanguages, artifactName);
}

}

bool PackBuildPlan::ready() const
{
    return diagnostics.empty() && std::all_of(variants.begin(), variants.end(),
        [](const auto& variant) {
            return variant.closure.ready() && variant.capability.permitted();
        });
}

QJsonObject PackBuildPlan::toJson() const
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
            {QStringLiteral("closure"), variant.closure.toJson()}});
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
