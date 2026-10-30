#include "IPCServerInternal.h"
#include "ComponentCatalog.h"
#include "ItemTypeCatalog.h"
#include "Tooling/Harkenizer/Core/PackBuildPlan.h"
#include "../PackGenerationRecord.h"
#include "../ProjectLanguageSemantics.h"
#include "../ProjectUpgradeManager.h"
#include "../ProjectUpgradeMessages.h"
#include "../RuntimeCapabilityMatrix.h"

#include <QDir>

namespace VNX {

void IPCServer::registerProjectHandlers()
{
    commandHandlers_["create_project"] = [this](const QJsonObject& args) { return handleCreateProject(args); };
    commandHandlers_["open_project"] = [this](const QJsonObject& args) { return handleOpenProject(args); };
    commandHandlers_["get_project_info"] = [this](const QJsonObject& args) { return handleGetProjectInfo(args); };

    commandHandlers_["list_packs"] = [this](const QJsonObject& args) { return handleListPacks(args); };

    commandHandlers_["list_items"] = [this](const QJsonObject& args) { return handleListItems(args); };
    commandHandlers_["get_item_info"] = [this](const QJsonObject& args) { return handleGetItemInfo(args); };
    commandHandlers_["get_action_pack_plan"] = [this](const QJsonObject& args) { return handleGetActionPackPlan(args); };
    commandHandlers_["get_runtime_capability_matrix"] = [this](const QJsonObject& args) { return handleGetRuntimeCapabilityMatrix(args); };
    commandHandlers_["get_last_pack_build_records"] = [this](const QJsonObject& args) { return handleGetLastPackBuildRecords(args); };

}

namespace {
constexpr qsizetype kMaximumPackPlanDimensionValues = 64;
constexpr qsizetype kMaximumPackPlanVariants = 64;

void setRuntimeStableIdField(QJsonObject& object, const ProjectItem* item)
{
    object.insert(
        QStringLiteral("runtime_stable_id"),
        objectIdString(item ? item->runtimeId() : harkenstudio::ObjectId{}));
}

ProjectItem* findStable(ProjectData* project, harkenstudio::ObjectId stableId)
{
    if (!project || harkenstudio::IsNull(stableId)) return nullptr;
    for (auto* binding : project->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        std::function<ProjectItem*(ProjectItem*)> visit = [&](ProjectItem* item) -> ProjectItem* {
            if (!item) return nullptr;
            if (item->runtimeId() == stableId) return item;
            for (auto& child : item->children)
                if (auto* found = visit(child.get())) return found;
            return nullptr;
        };
        if (auto* found = visit(binding->rootItem.get())) return found;
    }
    return nullptr;
}

bool parsePackPlanDimension(const QJsonObject& args, const QString& key,
                            const QStringList& allowed, QStringList* values,
                            QString* error)
{
    const QJsonValue input = args.value(key);
    if (!input.isUndefined() && !input.isArray()) {
        *error = QStringLiteral("%1 must be an array of strings").arg(key);
        return false;
    }
    const QJsonArray array = input.toArray();
    if (array.size() > kMaximumPackPlanDimensionValues) {
        *error = QStringLiteral("%1 exceeds the maximum of %2 values")
                     .arg(key).arg(kMaximumPackPlanDimensionValues);
        return false;
    }
    for (const auto& entry : array) {
        if (!entry.isString()) {
            *error = QStringLiteral("%1 must contain only strings").arg(key);
            return false;
        }
        const QString value = key == QStringLiteral("platform_ids") ? entry.toString() : entry.toString().trimmed();
        if (value.isEmpty() || !allowed.contains(value)) {
            *error = QStringLiteral("%1 contains unsupported value '%2'").arg(key, value);
            return false;
        }
        if (!values->contains(value)) values->append(value);
    }
    return true;
}
}

QJsonObject IPCServer::handleGetActionPackPlan(const QJsonObject& args)
{
    if (!projectManager_ || !projectManager_->isProjectOpen())
        return errorResponse(QStringLiteral("NO_PROJECT_OPEN"), QStringLiteral("No project is currently open"));
    auto* project = projectManager_->currentProject();
    harkenstudio::ObjectId stableId{};
    if (!parseObjectIdJson(args.value(QStringLiteral("actionStableId")), &stableId) || harkenstudio::IsNull(stableId))
        return errorResponse(QStringLiteral("INVALID_ARGS"),
                             QStringLiteral("actionStableId must be an object UUID"));
    const ProjectItem* action = findStable(project, stableId);
    if (!action || action->type != ProjectItem::Type::Action)
        return errorResponse(QStringLiteral("ACTION_NOT_FOUND"),
                             QStringLiteral("Action stable identity was not found"));

    if (args.contains("platforms") || args.contains("platform"))
        return errorResponse("INVALID_ARGS", "Use platform_ids with project platform UUIDs");
    const QStringList allowedPlatforms = project->platformIds();
    const QStringList allowedLanguages =
        ProjectLanguageSemantics::buildLanguages(*project);
    QStringList platforms;
    QStringList languages;
    QString dimensionError;
    if (!parsePackPlanDimension(args, QStringLiteral("platform_ids"), allowedPlatforms,
                                &platforms, &dimensionError)
        || !parsePackPlanDimension(args, QStringLiteral("languages"), allowedLanguages,
                                   &languages, &dimensionError))
        return errorResponse(QStringLiteral("INVALID_ARGS"), dimensionError);

    const qsizetype platformCount = platforms.isEmpty()
        ? allowedPlatforms.size() : platforms.size();
    const qsizetype requestedLanguageCount = languages.isEmpty()
        ? allowedLanguages.size() : languages.size();
    const qsizetype languageCount = requestedLanguageCount == 0 ? 1 : requestedLanguageCount;
    if (platformCount > kMaximumPackPlanVariants
        || languageCount > kMaximumPackPlanVariants
        || platformCount * languageCount > kMaximumPackPlanVariants)
        return errorResponse(
            QStringLiteral("PACK_PLAN_LIMIT_EXCEEDED"),
            QStringLiteral("Pack plan query exceeds the maximum of %1 variants")
                .arg(kMaximumPackPlanVariants));

    const auto plan = PackBuildPlanning::buildForAction(
        *project, stableId, platforms, languages, {}, false,
        projectManager_->packExportSelection());
    QJsonArray variants;
    for (const auto& variant : plan.variants) {
        if (variant.actionStableId != stableId)
            continue;
        QJsonObject value = variant.closure.toJson();
        value.insert(QStringLiteral("platform_id"), variant.variant.platform);
        value.insert(QStringLiteral("platform_name"), variant.platformDefinition.name);
        value.insert(QStringLiteral("base_platform"), ProjectPlatformSemantics::basePlatformName(variant.platformDefinition.basePlatform));
        value.insert(QStringLiteral("language"), variant.variant.language);
        value.insert(QStringLiteral("artifactName"), variant.artifactName);
        value.insert(QStringLiteral("relativePackPath"), variant.relativePackPath);
        value.insert(QStringLiteral("capability"), variant.capability.toJson());
        QJsonArray order;
        for (const auto id : variant.dependencyOrder)
            order.append(objectIdString(id));
        value.insert(QStringLiteral("dependencyOrder"), order);
        variants.append(value);
    }
    return successResponse(QJsonObject{{QStringLiteral("actionStableId"), objectIdString(stableId)},
                                       {QStringLiteral("variants"), variants}});
}

QJsonObject IPCServer::handleGetRuntimeCapabilityMatrix(const QJsonObject& args)
{
    harkenstudio::ObjectId stableId{};
    const auto value = args.contains(QStringLiteral("rootStableId"))
        ? args.value(QStringLiteral("rootStableId")) : args.value(QStringLiteral("actionStableId"));
    if (!value.isUndefined() && !parseObjectIdJson(value, &stableId))
        return errorResponse(QStringLiteral("INVALID_ARGS"), QStringLiteral("Expected an object UUID"));
    if (!harkenstudio::IsNull(stableId)) {
        if (!projectManager_ || !projectManager_->isProjectOpen())
            return errorResponse(QStringLiteral("NO_PROJECT_OPEN"), QStringLiteral("No project is currently open"));
        auto* project = projectManager_->currentProject();
        const auto operation = RuntimeCapabilityMatrix::parseOperation(
            args.value(QStringLiteral("operation")).toString(QStringLiteral("packBuild")));
        if (!operation)
            return errorResponse(QStringLiteral("INVALID_OPERATION"), QStringLiteral("Unknown runtime capability operation"));
        const QJsonObject variantJson = args.value(QStringLiteral("variant")).toObject();
        std::optional<BuildVariant> variant;
        if (!variantJson.isEmpty())
            variant = BuildVariant{variantJson.value(QStringLiteral("platform")).toString(),
                                   variantJson.value(QStringLiteral("language")).toString()};
        if (auto* item = findStable(project, stableId))
            return successResponse(RuntimeCapabilityMatrix::assess(
                *item, *project, *operation, variant).toJson());
        return errorResponse(QStringLiteral("OBJECT_NOT_FOUND"), QStringLiteral("Runtime stable identity was not found"));
    }
    return successResponse(RuntimeCapabilityMatrix::toJson());
}

QJsonObject IPCServer::handleGetLastPackBuildRecords(const QJsonObject& args)
{
    Q_UNUSED(args);
    QJsonArray records;
    for (const auto& record : PackGenerationRecordStore::instance().records())
        records.append(record.toJson());
    return successResponse(QJsonObject{{QStringLiteral("records"), records}});
}


QJsonObject IPCServer::handleCreateProject(const QJsonObject& args)
{
    ActionResult res;
    if (ActionManager::instance().hasAction(QStringLiteral("create_project")))
    {
        res = ActionManager::instance().execute(
            QStringLiteral("create_project"), args, ActionSource::IPC);
    }
    else
    {
        if (!projectManager_)
            return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE",
                                 "ProjectManager is not available");

        ProjectConfig config;
        config.name = args.value("name").toString();
        config.location = args.value("location").toString();
        if (config.name.isEmpty() || config.location.isEmpty())
            return errorResponse("INVALID_ARGS", "Required: name, location");

        config.sampleRate = args.value("sample_rate").toInt(48000);
        config.author = args.value("author").toString();
        config.description = args.value("description").toString();
        config.enableLFE = args.value("enable_lfe").toBool(true);

        const QString cc = args.value("channel_config").toString().toLower();
        if (cc == QStringLiteral("mono"))
            config.channelConfig = AudioChannelConfig::Mono;
        else if (cc == QStringLiteral("5_1"))
            config.channelConfig = AudioChannelConfig::Surround_5_1;
        else if (cc == QStringLiteral("7_1"))
            config.channelConfig = AudioChannelConfig::Surround_7_1;
        else if (cc == QStringLiteral("atmos_7_1_4"))
            config.channelConfig = AudioChannelConfig::Atmos_7_1_4;
        else
            config.channelConfig = AudioChannelConfig::Stereo_2_0;

        if (args.contains("target_platforms"))
            return errorResponse("UNSUPPORTED_PLATFORM_FORMAT", "Use platform instance definitions in 'platforms'");
        QString platformError;
        if (args.contains("platforms")
            && !ProjectPlatformSemantics::platformsFromJson(args.value("platforms"), &config.platforms, &platformError))
            return errorResponse("INVALID_ARGS", platformError);

        config.templateId = args.value("template_id").toString();
        if (args.value("template_data").isObject())
        {
            config.templateData = args.value("template_data").toObject();
        }
        else if (args.value("template_data").isString())
        {
            const auto doc = QJsonDocument::fromJson(
                args.value("template_data").toString().toUtf8());
            if (doc.isObject())
                config.templateData = doc.object();
        }

        const bool created = projectManager_->createProject(config);
        ProjectData* project = projectManager_->currentProject();
        res.success = created;
        if (created)
        {
            res.data = QJsonObject{
                {"success", true},
                {"path", project ? project->projectPath : QString()},
                {"template_applied", !config.templateId.isEmpty()
                                     && !config.templateData.isEmpty()}
            };
        }
        else
        {
            res.errorCode = QStringLiteral("CREATE_PROJECT_FAILED");
            res.errorMessage =
                QStringLiteral("Failed to create project (check location is writable)");
        }
    }

    if (!res.success) {
        return errorResponse(
            res.errorCode.isEmpty() ? QStringLiteral("CREATE_PROJECT_FAILED")
                                    : res.errorCode,
            res.errorMessage.isEmpty() ? QStringLiteral("Failed to create project")
                                       : res.errorMessage,
            QStringLiteral("Check name/location and that the location is writable."));
    }

    ProjectData* project = projectManager_ ? projectManager_->currentProject() : nullptr;
    if (!project)
        return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    if (mainWindow_)
        mainWindow_->loadProject(project->projectPath);

    emit projectOpenedViaIPC(project->projectPath);

    QJsonObject data = res.data;
    if (!data.contains(QStringLiteral("path")))
        data["path"] = project->projectPath;
    data["success"] = true;
    return successResponse(data);
}


QJsonObject IPCServer::handleOpenProject(const QJsonObject& args)
{
    if (!projectManager_)
    {
        return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE", 
                             "ProjectManager is not available");
    }
    
    QString path = args["path"].toString();
    if (path.isEmpty())
    {
        return errorResponse("INVALID_ARGS", "Project path is required");
    }
    
    if (!mainWindow_) {
        const ProjectUpgradeAnalysis analysis = ProjectUpgradeManager().analyze(path);
        if (analysis.ok && analysis.upgradeRequired)
            return errorResponse(
                projectIssueCode(ProjectIssue::ProjectUpgradeSelectionRequired),
                ProjectUpgradeMessages::userMessage(ProjectIssue::ProjectUpgradeSelectionRequired));
    }
    QString openError;
    bool forwarded = false;
    const bool success = mainWindow_
        ? mainWindow_->loadProject(path, {}, &openError, &forwarded)
        : projectManager_->loadProject(path, false, &openError, &forwarded);
    if (forwarded)
        return successResponse(QJsonObject{
            {QStringLiteral("forwarded"), true},
            {QStringLiteral("path"), path}
        });
    
    if (!success)
    {
        const QString code = openError.section(QLatin1Char(':'), 0, 0).trimmed();
        const ProjectIssue issue = projectIssueFromCode(code);
        if (issue != ProjectIssue::None && projectIssueCode(issue) == code)
            return errorResponse(code, ProjectUpgradeMessages::userMessage(openError, true));
        return errorResponse("OPEN_PROJECT_FAILED", 
                             "Failed to open project",
                             "Check if the project file exists and is valid.");
    }
    
    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");
    
    emit projectOpenedViaIPC(path);

    if (engine_ && engine_->isInitialized() && !engine_->isPlaying())
    {
        engine_->startOutput();
    }

    QJsonArray packs;
    for (const auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        packs.append(QJsonObject{
            {"name", binding->rootItem->name},
            {"path", project->objectFilePath(binding->rootItem.get())},
            {"guid", binding->rootItem->guid.toString(QUuid::WithoutBraces)}
        });
    }

    return successResponse(QJsonObject{
        {"name", project->name},
        {"path", project->projectPath},
        {"packs", packs}
    });
}




QJsonObject IPCServer::handleGetProjectInfo(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!projectManager_)
    {
        return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE", 
                             "ProjectManager is not available");
    }
    
    if (!projectManager_->isProjectOpen())
    {
        return successResponse(QJsonObject{
            {"project_open", false}
        });
    }
    
    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    QJsonArray packs;
    for (const auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        int childrenCount = static_cast<int>(binding->rootItem->children.size());
        int mediaCount = static_cast<int>(collectMediaFiles(binding->rootItem.get()).size());
        packs.append(QJsonObject{
            {"name", binding->rootItem->name},
            {"path", project->objectFilePath(binding->rootItem.get())},
            {"parent_folder", binding->parentFolder},
            {"guid", binding->rootItem->guid.toString(QUuid::WithoutBraces)},
            {"is_dirty", binding->rootItem->isDirtyObject()},
            {"children_count", childrenCount},
            {"media_count", mediaCount}
        });
    }

    QJsonArray actions;
    for (const QString& actionName : project->actionNames)
    {
        actions.append(actionName);
    }

    const QJsonArray platformsArr = ProjectPlatformSemantics::platformsToJson(project->platforms);

    return successResponse(QJsonObject{
        {"project_open", true},
        {"name", project->name},
        {"path", project->projectPath},
        {"version", project->version},
        {"author", project->author},
        {"description", project->description},
        {"packs", packs},
        {"actions", actions},
        {"audio_folder", project->audioFolder},
        {"pack_folder", project->packFolder},
        {"platforms", platformsArr},
        {"sample_rate", project->sampleRate},
        {"channel_config", static_cast<int>(project->channelConfig)},
        {"enable_lfe", project->enableLFE},
        {"project_template_id", project->projectTemplateId}
    });
}




QJsonObject IPCServer::handleListPacks(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!projectManager_)
    {
        return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE", 
                             "ProjectManager is not available");
    }
    
    if (!projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN", 
                             "No project is currently open");
    }
    
    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");
    
    QJsonArray packs;
    for (const auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        packs.append(QJsonObject{
            {"name", binding->rootItem->name},
            {"path", project->objectFilePath(binding->rootItem.get())},
            {"parent_folder", binding->parentFolder},
            {"guid", binding->rootItem->guid.toString(QUuid::WithoutBraces)},
            {"is_dirty", binding->rootItem->isDirtyObject()}
        });
    }

    return successResponse(QJsonObject{{"packs", packs}});
}

ProjectItem* findItemByPath(ProjectItem* root, const QString& path)
{
    if (!root) return nullptr;
    std::vector<ProjectItem*> matches{root};
    for (const auto& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        std::vector<ProjectItem*> next;
        for (const auto* item : matches)
            for (const auto& child : item->children)
                if (child && child->name == part) next.push_back(child.get());
        matches = std::move(next);
    }
    return matches.size() == 1 ? matches.front() : nullptr;
}

ProjectItem* findItemByNameRecursive(ProjectItem* item, const QString& name)
{
    if (!item) return nullptr;
    ProjectItem* match = nullptr;
    std::vector<ProjectItem*> pending{item};
    while (!pending.empty()) {
        auto* current = pending.back();
        pending.pop_back();
        if (!current) continue;
        if (current->name == name) {
            if (match) return nullptr;
            match = current;
        }
        for (const auto& child : current->children) pending.push_back(child.get());
    }
    return match;
}

ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path)
{
    return project ? project->findItemBySelector(path) : nullptr;
}

ProjectFileBinding* findBindingByFolder(ProjectData* project, const QString& folder)
{
    if (!project) return nullptr;
    for (auto* binding : project->allFileBindings())
    {
        if (binding && binding->parentFolder == folder)
            return binding;
    }
    return nullptr;
}


uint32_t itemShortID(const ProjectItem* item)
{
    return item ? fnv1a_hash(item->name) : 0u;
}

ProjectItem* findItemByShortID(ProjectItem* root, uint32_t shortID)
{
    if (!root || shortID == 0) return nullptr;
    ProjectItem* match = nullptr;
    std::vector<ProjectItem*> pending{root};
    while (!pending.empty()) {
        auto* item = pending.back();
        pending.pop_back();
        if (!item) continue;
        if (itemShortID(item) == shortID) {
            if (match) return nullptr;
            match = item;
        }
        for (const auto& child : item->children) pending.push_back(child.get());
    }
    return match;
}

static std::vector<ProjectItem*> findItemsByShortIDInProject(ProjectData* project, uint32_t shortID)
{
    std::vector<ProjectItem*> matches;
    if (!project || shortID == 0) return matches;
    std::vector<ProjectItem*> pending;
    for (const auto* binding : project->allFileBindings()) {
        if (binding) pending.push_back(binding->rootItem.get());
    }
    while (!pending.empty()) {
        auto* item = pending.back();
        pending.pop_back();
        if (!item) continue;
        if (itemShortID(item) == shortID) matches.push_back(item);
        for (const auto& child : item->children) pending.push_back(child.get());
    }
    return matches;
}

ProjectItem* findItemByShortIDInProject(ProjectData* project, uint32_t shortID)
{
    const auto matches = findItemsByShortIDInProject(project, shortID);
    return matches.size() == 1 ? matches.front() : nullptr;
}

ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID)
{
    if (!project || objectID == 0) return nullptr;

    const auto matches = findItemsByShortIDInProject(project, objectID);
    if (!matches.empty()) return matches.size() == 1 ? matches.front() : nullptr;

    return findItemByAuthoringHandleInProject(project, objectID);
}

std::vector<ProjectItem*> findItemsByObjectIDInProject(
    ProjectData* project, std::span<const uint32_t> objectIDs)
{
    std::vector<ProjectItem*> result(objectIDs.size(), nullptr);
    if (!project || objectIDs.empty()) return result;

    struct Matches
    {
        ProjectItem* shortId = nullptr;
        ProjectItem* authoringHandle = nullptr;
        bool ambiguous = false;
    };
    QHash<uint32_t, Matches> matches;
    for (const auto id : objectIDs)
        if (id != 0) matches.insert(id, {});

    const auto collect = [&](auto&& self, ProjectItem* item) -> void {
        if (!item) return;
        if (auto match = matches.find(itemShortID(item)); match != matches.end()) {
            match->ambiguous = match->shortId != nullptr;
            match->shortId = item;
        }
        if (auto match = matches.find(item->objectID); match != matches.end()) {
            if (!match->authoringHandle) match->authoringHandle = item;
        }
        for (const auto& child : item->children) self(self, child.get());
    };
    for (const auto* binding : project->allFileBindings())
        if (binding) collect(collect, binding->rootItem.get());

    for (size_t index = 0; index < objectIDs.size(); ++index) {
        const auto match = matches.constFind(objectIDs[index]);
        if (match == matches.cend() || match->ambiguous) continue;
        result[index] = match->shortId ? match->shortId : match->authoringHandle;
    }
    return result;
}


bool containsProjectItemPointer(const ProjectItem* root, const ProjectItem* target)
{
    if (!root || !target) return false;
    if (root == target) return true;

    for (const auto& child : root->children)
    {
        if (containsProjectItemPointer(child.get(), target))
        {
            return true;
        }
    }
    return false;
}

ProjectFileBinding* findOwnerBindingForItem(ProjectData* project, ProjectItem* item)
{
    if (!project || !item) return nullptr;

    if (auto* binding = findFileBindingForItem(*project, item))
    {
        return binding;
    }

    for (auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        if (containsProjectItemPointer(binding->rootItem.get(), item))
        {
            return binding;
        }
    }

    return nullptr;
}


QString buildItemFullPath(ProjectData* project, ProjectItem* item)
{
    return project ? project->itemPath(item) : QString();
}

ProjectItem* resolveItemByPathOrObjectID(ProjectData* project,
                                                const QString& path,
                                                uint32_t objectID)
{
    if (!project) return nullptr;
    if (!path.isEmpty())
    {
        return findItemByPathInProject(project, path);
    }
    if (objectID != 0)
    {
        // Accept both identifiers callers receive in read responses: the
        // name-hash short id (object_id) and the authoring handle
        // (authoring_handle). Short id is tried first, so this only adds
        // resolution for handles that previously failed.
        return findItemByObjectIDInProject(project, objectID);
    }
    return nullptr;
}


QString itemTypeToString(ProjectItem::Type type)
{
    return ItemTypeCatalog::ipcName(type);
}

std::optional<ProjectItem::Type> stringToItemType(const QString& typeStr)
{
    return ItemTypeCatalog::parseTypeName(typeStr, Qt::CaseSensitive, true);
}




QJsonObject IPCServer::handleListItems(const QJsonObject& args)
{
    if (!projectManager_)
    {
        return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE", 
                             "ProjectManager is not available");
    }
    
    if (!projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN", 
                             "No project is currently open");
    }
    
    QString parentPath = args["parent"].toString();
    uint32_t parentID = static_cast<uint32_t>(args["parent_id"].toInteger(0));

    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    if (!parentPath.isEmpty() && parentID != 0)
    {
        ProjectItem* byPath = findItemByPathInProject(project, parentPath);
        ProjectItem* byID = findItemByShortIDInProject(project, parentID);
        if (byPath && byID && byPath != byID)
        {
            return errorResponse("INVALID_ARGS",
                                 "parent and parent_id refer to different items");
        }
    }

    if (parentPath.isEmpty() && parentID == 0) {
        QJsonArray items;
        for (const auto* binding : project->allFileBindings()) {
            if (binding && binding->rootItem) {
                const QString fullPath = buildItemFullPath(project, binding->rootItem.get());
                QJsonObject listed{
                    {"name", binding->rootItem->name},
                    {"type", itemTypeToString(binding->rootItem->type)},
                    {"path", fullPath},
                    {"full_path", fullPath},
                    {"has_children", !binding->rootItem->children.empty()},
                    {"source_id", static_cast<qint64>(binding->rootItem->sourceID)},
                    {"object_id", static_cast<qint64>(itemShortID(binding->rootItem.get()))},
                    {"authoring_handle", static_cast<qint64>(binding->rootItem->objectID)}
                };
                setRuntimeStableIdField(listed, binding->rootItem.get());
                items.append(listed);
            }
        }
        return successResponse(QJsonObject{{"items", items}});
    }

    ProjectItem* parent = resolveItemByPathOrObjectID(project, parentPath, parentID);

    if (!parent)
    {
        return errorResponse("PARENT_NOT_FOUND",
                             parentID != 0
                                 ? QString("Parent item with object_id '%1' not found").arg(parentID)
                                 : QString("Parent item '%1' not found").arg(parentPath));
    }

    QJsonArray items;
    for (const auto& child : parent->children)
    {
        const QString fullPath = buildItemFullPath(project, child.get());
        QJsonObject listed{
            {"name", child->name},
            {"type", itemTypeToString(child->type)},
            {"path", fullPath},
            {"full_path", fullPath},
            {"asset_path", child->type == ProjectItem::Type::Sound
                               ? child->sourcePath : QString{}},
            {"has_children", !child->children.empty()},
            {"source_id", static_cast<qint64>(child->sourceID)},
            {"object_id", static_cast<qint64>(itemShortID(child.get()))},
            {"authoring_handle", static_cast<qint64>(child->objectID)}
        };
        setRuntimeStableIdField(listed, child.get());
        items.append(listed);
    }

    return successResponse(QJsonObject{{"items", items}});
}


namespace {
QString componentTagName(ComponentType type)
{
    return ComponentCatalog::componentName(type);
}
}

QJsonObject IPCServer::handleGetItemInfo(const QJsonObject& args)
{
    if (!projectManager_)
    {
        return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE", 
                             "ProjectManager is not available");
    }
    
    if (!projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN", 
                             "No project is currently open");
    }
    
    QString path = args["path"].toString();
    uint32_t objectID = static_cast<uint32_t>(args["object_id"].toInteger(0));

    if (path.isEmpty() && objectID == 0)
    {
        return errorResponse("INVALID_ARGS", "path or object_id is required");
    }
    if (!path.isEmpty() && objectID != 0)
    {
        ProjectItem* byPath = findItemByPathInProject(projectManager_->currentProject(), path);
        ProjectItem* byID = findItemByShortIDInProject(projectManager_->currentProject(), objectID);
        if (byPath && byID && byPath != byID)
        {
            return errorResponse("INVALID_ARGS", "path and object_id refer to different items");
        }
    }
    
    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    ProjectItem* item = resolveItemByPathOrObjectID(project, path, objectID);

    if (!item)
    {
        return errorResponse("ITEM_NOT_FOUND",
                             objectID != 0
                                 ? QString("Item with object_id '%1' not found").arg(objectID)
                                 : QString("Item '%1' not found").arg(path));
    }

    QJsonArray children;
    for (const auto& child : item->children)
    {
        const QString childFullPath = buildItemFullPath(project, child.get());
        QJsonObject listed{
            {"name", child->name},
            {"type", itemTypeToString(child->type)},
            {"source_id", static_cast<qint64>(child->sourceID)},
            {"object_id", static_cast<qint64>(itemShortID(child.get()))},
            {"authoring_handle", static_cast<qint64>(child->objectID)},
            {"path", childFullPath}
        };
        setRuntimeStableIdField(listed, child.get());
        children.append(listed);
    }

    QJsonObject info{
        {"name", item->name},
        {"type", itemTypeToString(item->type)},
        {"path", buildItemFullPath(project, item)},
        {"asset_path", item->type == ProjectItem::Type::Sound
                           ? item->sourcePath
                           : item->parent ? QString{} : project->itemFolderPath(item)},
        {"object_id", static_cast<qint64>(itemShortID(item))},
        {"authoring_handle", static_cast<qint64>(item->objectID)},
        {"source_id", static_cast<qint64>(item->sourceID)},
        {"children", children}
    };
    setRuntimeStableIdField(info, item);

    QJsonObject components;
    item->components.forEach([&](const IComponent& c) {
        components.insert(componentTagName(c.getType()), c.toJson());
    });
    if (!components.isEmpty())
        info.insert("components", components);

    return successResponse(info);
}


}
