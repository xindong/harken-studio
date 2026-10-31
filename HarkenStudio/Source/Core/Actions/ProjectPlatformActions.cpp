#include "Core/Components/VSelection.h"
#include "../ActionManager.h"
#include "../ProjectManager.h"
#include "../ProjectPlatform.h"
#include "../ProjectPlatformCommandSchema.h"
#include "../PlatformContextService.h"
#include "../PropertyEditSession.h"
#include "../FieldGroupProjectAdapter.h"
#include "../Components/VTrack.h"
#include "../SoundSourceSemantics.h"

namespace VNX {
namespace {

namespace Platforms = ProjectPlatformSemantics;

enum class Operation { Create, Rename, Delete, Base, Profile, Batch };

struct RootState
{
    QVector<ProjectPlatform> platforms;
    QString current;
    QStringList selected;
    std::vector<ActionPackTargeting> targeting;
    PlatformContext context;
};

struct OwnerState
{
    QUuid guid;
    PlatformOverrideLedgerV1 definition;
    DefinitionFieldGroups fieldGroups;
    QMap<QUuid, PlatformOverrideLedgerV1> clips;
    std::optional<SelectionData> selections;
    std::optional<SoundSources> sources;

    bool operator==(const OwnerState&) const = default;
};

struct EditState
{
    RootState root;
    QVector<OwnerState> owners;
};

RootState captureRoot(const ProjectData& project)
{
    const auto selection = ProjectManager::instance().packExportSelection();
    return {project.platforms, selection.currentPlatformId,
            selection.selectedPlatformIds, selection.actionTargeting,
            PlatformContextService::instance().context()};
}

OwnerState captureOwner(const ProjectItem& item)
{
    OwnerState state;
    state.guid = item.guid;
    state.definition = item.platformOverrides;
    state.fieldGroups = item.fieldGroups;
    if (const auto* selection = item.comp<VSelection>()) state.selections = selection->selections;
    if (const auto* track = item.comp<VTrack>()) {
        for (const auto& clip : track->clips.items)
            state.clips.insert(clip.clipGuid, clip.platformOverrides);
    }
    if (const auto* sound = item.comp<VSound>()) state.sources = sound->sources;
    return state;
}

void removeReferences(OwnerState& owner, const ProjectPlatformId& id)
{
    owner.definition.erase(id);
    const auto scope = toFieldGroupGuid(id);
    owner.fieldGroups.mainOutput.platforms.erase(scope);
    owner.fieldGroups.send.platforms.erase(scope);
    owner.fieldGroups.spatialRating.platforms.erase(scope);
    for (auto& ledger : owner.clips) ledger.erase(id);
    if (owner.selections) owner.selections->platforms.remove(Platforms::idString(id));
    if (owner.sources)
        for (auto* set : SoundSourceSemantics::sourceSets(*owner.sources))
            set->platforms.remove(Platforms::idString(id));
}

void reconcileContext(RootState& state)
{
    auto& context = state.context;
    const auto ids = occupiedTargetPlatforms(Platforms::ids(state.platforms));
    if (!ids.contains(context.primary)) context.primary = ids.first();
    context.selected = context.all ? ids : QVector<ProjectPlatformId>{context.primary};
}

bool canApply(const ProjectManager& manager, const EditState& state)
{
    for (const auto& owner : state.owners) {
        const auto* item = manager.findItemByGuid(owner.guid);
        if (!item) return false;
        if (owner.sources && !item->comp<VSound>()) return false;
        if (owner.selections && !item->comp<VSelection>()) return false;
        const auto* track = item->comp<VTrack>();
        for (auto it = owner.clips.begin(); it != owner.clips.end(); ++it) {
            if (!track || std::none_of(track->clips.items.begin(), track->clips.items.end(),
                                      [&](const auto& clip) { return clip.clipGuid == it.key(); }))
                return false;
        }
    }
    return true;
}

void apply(ProjectManager& manager, const EditState& state, const QStringList& platformIds,
           bool membershipChanged)
{
    PropertyEditSession::finishAllActive(false);
    auto& project = *manager.currentProject();
    const auto previousPlatformIds = project.platformIds();
    project.platforms = state.root.platforms;
    if (membershipChanged) {
        auto selection = manager.packExportSelection();
        const auto nextPlatformIds = project.platformIds();
        if (!nextPlatformIds.contains(selection.currentPlatformId))
            selection.currentPlatformId = state.root.current;
        selection.selectedPlatformIds.removeIf([&](const auto& id) {
            return !nextPlatformIds.contains(id);
        });
        for (const auto& id : state.root.selected)
            if (!previousPlatformIds.contains(id) && nextPlatformIds.contains(id))
                selection.selectedPlatformIds.append(id);
        const auto currentTargeting = selection.actionTargeting;
        if (std::any_of(nextPlatformIds.begin(), nextPlatformIds.end(),
            [&](const auto& id) { return !previousPlatformIds.contains(id); })) {
            for (const auto& saved : state.root.targeting)
                if (std::none_of(selection.actionTargeting.begin(), selection.actionTargeting.end(),
                    [&](const auto& entry) {
                        return entry.stableId == saved.stableId && entry.language == saved.language;
                    }))
                    selection.actionTargeting.push_back(saved);
        }
        const auto effectiveEntry = [](const auto& entries, const auto& key) -> const ActionPackTargeting* {
            const ActionPackTargeting* inherited = nullptr;
            for (const auto& entry : entries) {
                if (entry.stableId != key.stableId) continue;
                if (entry.language == key.language) return &entry;
                if (!entry.language) inherited = &entry;
            }
            return inherited;
        };
        for (auto& entry : selection.actionTargeting) {
            const auto* current = effectiveEntry(currentTargeting, entry);
            const auto* saved = effectiveEntry(state.root.targeting, entry);
            QStringList candidates = current ? current->platformIds : QStringList{};
            if (saved) candidates.append(saved->platformIds);
            candidates.append(nextPlatformIds);
            candidates.removeDuplicates();
            entry.allPlatforms = !current || current->allPlatforms;
            entry.platformIds.clear();
            for (const auto& id : candidates) {
                if (!nextPlatformIds.contains(id)) continue;
                const auto* source = previousPlatformIds.contains(id) ? current : saved;
                if (!source || source->includesPlatform(id)) entry.platformIds.append(id);
                else entry.allPlatforms = false;
            }
            if (entry.allPlatforms) entry.platformIds.clear();
        }
        manager.setPackExportSelectionInternal(std::move(selection));
    }
    QVector<ProjectItem*> changed;
    for (const auto& owner : state.owners) {
        auto* item = manager.findItemByGuid(owner.guid);
        item->platformOverrides = owner.definition;
        item->fieldGroups = owner.fieldGroups;
        if (owner.selections) item->comp<VSelection>()->selections = *owner.selections;
        if (auto* track = item->comp<VTrack>()) {
            for (auto& clip : track->clips.items) {
                const auto saved = owner.clips.constFind(clip.clipGuid);
                if (saved != owner.clips.constEnd()) clip.platformOverrides = *saved;
            }
        }
        if (owner.sources) item->comp<VSound>()->sources = *owner.sources;
        item->markDirty();
        changed.append(item);
    }
    if (membershipChanged) {
        const auto& context = state.root.context;
        PlatformContextService::instance().setSelected(context.selected, context.primary, context.all);
    }
    manager.notifyProjectPlatformEdits(changed, platformIds);
}

ActionResult commitEdit(ProjectManager* manager, EditState before, EditState after,
                        Operation operation, const QStringList& editedIds, ActionResult result)
{
    PropertyEditSession::finishAllActive(true);
    const auto project = manager->currentProjectLease();
    before.root = captureRoot(*project);
    after.root.context = before.root.context;
    reconcileContext(after.root);
    QStringList removedIds;
    for (const auto& id : Platforms::ids(before.root.platforms))
        if (!Platforms::ids(after.root.platforms).contains(id)) removedIds.append(id);
    const bool membershipChanged = Platforms::ids(before.root.platforms) != Platforms::ids(after.root.platforms);
    if (!removedIds.isEmpty()) {
        for (const auto* item : Platforms::referenceOwners(*project)) {
            auto old = captureOwner(*item);
            auto removed = old;
            for (const auto& id : removedIds) removeReferences(removed, QUuid(id));
            if (old == removed) continue;
            before.owners.append(std::move(old));
            after.owners.append(std::move(removed));
        }
    }
    const std::weak_ptr<ProjectData> lease = project;
    const auto replay = [manager, lease, editedIds, membershipChanged](const EditState& state) {
        const auto project = lease.lock();
        if (project && manager->currentProject() == project.get() && canApply(*manager, state))
            apply(*manager, state, editedIds, membershipChanged);
    };
    auto history = std::make_shared<std::pair<EditState, EditState>>(std::move(before), std::move(after));
    apply(*manager, history->second, editedIds, membershipChanged);
    result.undoFn = [replay, history]() { replay(history->first); };
    result.redoFn = [replay, history]() { replay(history->second); };
    result.canReplay = [manager, lease]() {
        const auto project = lease.lock();
        return project && manager->currentProject() == project.get();
    };
    result.undoText = operation == Operation::Create ? QStringLiteral("Create Project Platform")
        : operation == Operation::Rename ? QStringLiteral("Rename Project Platform")
        : operation == Operation::Delete ? QStringLiteral("Delete Project Platform")
        : operation == Operation::Base ? QStringLiteral("Change Base Platform")
        : operation == Operation::Profile ? QStringLiteral("Set Project Platform Profile")
        : QStringLiteral("Apply Project Platforms");
    result.data.insert("affected_item_count", history->second.owners.size());
    return result;
}

ActionResult executeEdit(ProjectManager* manager, Operation operation, const QJsonObject& args)
{
    if (!manager || !manager->isProjectOpen())
        return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

    QStringList allowed;
    if (operation == Operation::Create) allowed = {"name", "base_platform", "profile"};
    if (operation == Operation::Rename) allowed = {"platform_id", "name"};
    if (operation == Operation::Delete) allowed = {"platform_id"};
    if (operation == Operation::Base) allowed = {"platform_id", "base_platform"};
    if (operation == Operation::Profile) allowed = {"platform_id", "profile"};
    for (const auto& key : args.keys()) {
        if (!allowed.contains(key))
            return {false, {}, "INVALID_ARGS", QStringLiteral("Unknown platform argument: %1").arg(key)};
    }

    const auto project = manager->currentProjectLease();
    QString error;
    if (!Platforms::validateProject(*project, &error))
        return {false, {}, "INVALID_PROJECT", error};
    EditState before{captureRoot(*project), {}};
    EditState after = before;
    ProjectPlatform definition;
    qsizetype index = -1;
    if (operation != Operation::Create) {
        const auto id = Platforms::parseId(args.value("platform_id").toString());
        if (!id) return {false, {}, "INVALID_ARGS", "platform_id must be a project platform UUID"};
        for (qsizetype i = 0; i < after.root.platforms.size(); ++i) {
            if (after.root.platforms[i].id == *id) index = i;
        }
        if (index < 0) return {false, {}, "PLATFORM_NOT_FOUND", "Platform ID is not defined in this project"};
        definition = after.root.platforms[index];
    }

    if (operation == Operation::Create || operation == Operation::Rename) {
        if (!args.value("name").isString() || !Platforms::validateName(args.value("name").toString(), &error))
            return {false, {}, "INVALID_ARGS", error.isEmpty() ? QStringLiteral("name must be a string") : error};
        definition.name = args.value("name").toString();
    }
    if (operation == Operation::Create || operation == Operation::Base) {
        const auto base = Platforms::parseBasePlatform(args.value("base_platform").toString());
        if (!base) return {false, {}, "INVALID_ARGS", "base_platform must be Windows, macOS, Linux, Android, or iOS"};
        if (operation == Operation::Create) definition.id = QUuid::createUuid();
        if (operation == Operation::Create || definition.basePlatform != *base)
            definition.profile = ProjectPlatformProfile::fromBasePlatform(*base);
        definition.basePlatform = *base;
    }
    if (operation == Operation::Profile || args.contains("profile")) {
        if (!args.contains("profile")) return {false, {}, "INVALID_ARGS", "profile must be an object or null"};
        const auto profile = args.value("profile");
        if (profile.isNull()) definition.profile = ProjectPlatformProfile::fromBasePlatform(definition.basePlatform);
        else {
            auto json = definition.toJson();
            json.insert("profile", profile);
            QVector<ProjectPlatform> parsed;
            if (!Platforms::platformsFromJson(QJsonArray{json}, &parsed, &error))
                return {false, {}, "INVALID_ARGS", error};
            definition.profile = parsed.first().profile;
        }
    }

    const auto id = Platforms::idString(definition.id);
    if (operation == Operation::Create) {
        after.root.platforms.append(definition);
        after.root.selected.append(id);
    } else if (operation == Operation::Delete) {
        if (after.root.platforms.size() == 1)
            return {false, {}, "LAST_PLATFORM", "The last project platform cannot be deleted"};
        after.root.platforms.removeAt(index);
        after.root.selected.removeAll(id);
        if (after.root.current == id) after.root.current = Platforms::idString(after.root.platforms.first().id);
        for (auto& targeting : after.root.targeting) targeting.platformIds.removeAll(id);
    } else {
        after.root.platforms[index] = definition;
    }
    if (!Platforms::validatePlatforms(after.root.platforms, &error))
        return {false, {}, "INVALID_ARGS", error};
    ActionResult result;
    result.success = true;
    result.data = {{"platform_id", id}, {"platform", definition.toJson()}};
    if (Platforms::platformsToJson(before.root.platforms) == Platforms::platformsToJson(after.root.platforms))
        return result;

    return commitEdit(manager, std::move(before), std::move(after), operation, {id}, std::move(result));
}

ActionResult applyPlatforms(ProjectManager* manager, const QJsonObject& args)
{
    if (!manager || !manager->isProjectOpen())
        return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
    if (args.keys() != QStringList{"expected_platforms", "platforms"}
        || !args.value("platforms").isArray() || !args.value("expected_platforms").isArray())
        return {false, {}, "INVALID_ARGS", "Provide platforms and expected_platforms arrays"};
    const auto* project = manager->currentProject();
    if (args.value("expected_platforms").toArray() != Platforms::platformsToJson(project->platforms))
        return {false, {}, "PLATFORM_EDIT_CONFLICT", "Project platforms changed. Reload settings before applying."};
    QString error;
    if (!Platforms::validateProject(*project, &error))
        return {false, {}, "INVALID_PROJECT", error};
    QJsonArray rows;
    for (const auto& value : args.value("platforms").toArray()) {
        if (!value.isObject()) return {false, {}, "INVALID_ARGS", "Platform definitions must be objects"};
        auto row = value.toObject();
        if (!row.contains("id")) row.insert("id", Platforms::idString(QUuid::createUuid()));
        else {
            const auto* existing = Platforms::find(project->platforms, QUuid(row.value("id").toString()));
            if (!existing)
                return {false, {}, "PLATFORM_NOT_FOUND", "Omit id when creating a platform"};
        }
        rows.append(row);
    }
    QVector<ProjectPlatform> platforms;
    if (!Platforms::platformsFromJson(rows, &platforms, &error))
        return {false, {}, "INVALID_ARGS", error};
    ActionResult result{true, {{"platforms", rows}}};
    if (rows == Platforms::platformsToJson(project->platforms)) return result;
    EditState before{captureRoot(*project), {}};
    EditState after = before;
    after.root.platforms = platforms;
    const auto ids = Platforms::ids(platforms);
    const auto oldIds = Platforms::ids(project->platforms);
    QStringList editedIds;
    for (const auto& platform : project->platforms) {
        const auto id = Platforms::idString(platform.id);
        const auto* next = Platforms::find(platforms, platform.id);
        if (next && next->toJson() == platform.toJson()) continue;
        editedIds.append(id);
        if (next) continue;
        after.root.selected.removeAll(id);
        if (after.root.current == id) after.root.current = ids.first();
        for (auto& targeting : after.root.targeting) targeting.platformIds.removeAll(id);
    }
    for (const auto& id : ids) {
        if (oldIds.contains(id)) continue;
        after.root.selected.append(id);
        editedIds.append(id);
    }
    return commitEdit(manager, std::move(before), std::move(after), Operation::Batch, editedIds, std::move(result));
}

}

void registerProjectPlatformActions(ActionManager& manager, ProjectManager* projectManager)
{
    ActionDef list;
    list.id = QStringLiteral("list_project_platforms");
    list.displayName = QStringLiteral("List Project Platforms");
    list.category = QStringLiteral("project");
    list.undoable = false;
    list.paramSchema = projectPlatformCommandSchema(list.id);
    list.handler = [projectManager](const QJsonObject& args) -> ActionResult {
        if (!args.isEmpty()) return {false, {}, "INVALID_ARGS", "This query takes no arguments"};
        if (!projectManager || !projectManager->isProjectOpen())
            return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
        const auto& platforms = projectManager->currentProject()->platforms;
        QJsonObject profiles;
        for (const auto& platform : platforms)
            profiles.insert(Platforms::idString(platform.id), platform.profile.toJson());
        return {true, {{"platforms", Platforms::platformsToJson(platforms)}, {"effective_profiles", profiles},
                       {"base_platforms", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}, {}, {}};
    };
    manager.registerAction(list);
    ActionDef batch;
    batch.id = QStringLiteral("apply_project_platforms");
    batch.displayName = QStringLiteral("Apply Project Platforms");
    batch.category = QStringLiteral("project");
    batch.paramSchema = projectPlatformCommandSchema(batch.id);
    batch.handler = [projectManager](const QJsonObject& args) { return applyPlatforms(projectManager, args); };
    manager.registerAction(batch);
    const auto add = [&](const QString& id, const QString& name, Operation operation) {
        ActionDef definition;
        definition.id = id;
        definition.displayName = name;
        definition.category = QStringLiteral("project");
        definition.paramSchema = projectPlatformCommandSchema(id);
        definition.handler = [projectManager, operation](const QJsonObject& args) {
            return executeEdit(projectManager, operation, args);
        };
        manager.registerAction(definition);
    };
    add(QStringLiteral("create_project_platform"), QStringLiteral("Create Project Platform"), Operation::Create);
    add(QStringLiteral("rename_project_platform"), QStringLiteral("Rename Project Platform"), Operation::Rename);
    add(QStringLiteral("delete_project_platform"), QStringLiteral("Delete Project Platform"), Operation::Delete);
    add(QStringLiteral("set_project_platform_base"), QStringLiteral("Change Base Platform"), Operation::Base);
    add(QStringLiteral("set_project_platform_profile"), QStringLiteral("Set Project Platform Profile"), Operation::Profile);
}

}
