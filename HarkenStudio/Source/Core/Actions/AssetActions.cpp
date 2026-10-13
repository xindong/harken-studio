
#include "../ActionManager.h"
#include "../AudioEngine.h"
#include "../ProjectManager.h"
#include "../ProjectLanguageSemantics.h"
#include "../UndoCommands.h"
#include "../AssetPipeline/ImportAssetsCommand.h"
#include "../AssetPipeline/ImportSoundSourcesCommand.h"
#include "../AssetPipeline/AudioImportGate.h"
#include "../AssetPipeline/AssetRegistry.h"
#include "../AssetPipeline/AssetConverter.h"
#include "../AssetPipeline/AssetGarbageCollector.h"
#include "../AssetPipeline/PlatformProfile.h"
#include "../AssetPipeline/MediaPool.h"
#include "Blueprint/BlueprintImportUtils.h"
#include "ActionSelectors.h"
#include "RuntimeBridge/PackagingBridge.h"
#include <QJsonArray>

namespace VNX { ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path); }
namespace VNX {
ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
uint32_t itemShortID(const ProjectItem* item);
}

namespace VNX {


static QJsonArray sourceIDArrayFromArgs(const QJsonObject& args)
{
    QJsonArray arr = args.value(QStringLiteral("source_ids")).toArray();
    if (arr.isEmpty()) {
        arr = args.value(QStringLiteral("sourceIDs")).toArray();
    }
    return arr;
}

static QMap<uint32_t, QString> replacementMapFromArgs(const QJsonObject& args)
{
    QMap<uint32_t, QString> replacements;

    const QJsonObject replacementsObj = args.value(QStringLiteral("replacements")).toObject();
    for (auto it = replacementsObj.begin(); it != replacementsObj.end(); ++it) {
        uint32_t id = it.key().toUInt();
        QString path = it.value().toString();
        if (id != 0 && !path.isEmpty()) {
            replacements[id] = path;
        }
    }

    const QJsonArray items = args.value(QStringLiteral("items")).toArray();
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        uint32_t id = static_cast<uint32_t>(item.value(QStringLiteral("source_id")).toInteger());
        if (id == 0) {
            id = static_cast<uint32_t>(item.value(QStringLiteral("sourceID")).toInteger());
        }
        const QString path = item.value(QStringLiteral("path")).toString(
            item.value(QStringLiteral("new_path")).toString());
        if (id != 0 && !path.isEmpty()) {
            replacements[id] = path;
        }
    }

    return replacements;
}

void registerAssetActions(ActionManager& mgr,
                          AudioEngine* engine,
                          ProjectManager* projectManager)
{
    (void)projectManager;

    {
        ActionDef def;
        def.id = "add_project_language";
        def.displayName = "Add Project Language";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            if (!ProjectManager::instance().isProjectOpen())
                return {false, {}, "NO_PROJECT", "No project open"};
            QString name = args["name"].toString();
            if (name.isEmpty()) return {false, {}, "MISSING_PARAM", "'name' required"};
            bool ok = ProjectManager::instance().addProjectLanguage(name);
            return {true, QJsonObject{{"success", ok}, {"language", name}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "remove_project_language";
        def.displayName = "Remove Project Language";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            if (!ProjectManager::instance().isProjectOpen())
                return {false, {}, "NO_PROJECT", "No project open"};
            QString name = args["name"].toString();
            if (name.isEmpty()) return {false, {}, "MISSING_PARAM", "'name' required"};
            bool ok = ProjectManager::instance().removeProjectLanguage(name);
            return {true, QJsonObject{{"success", ok}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_default_language";
        def.displayName = "Set Default Language";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            if (!ProjectManager::instance().isProjectOpen())
                return {false, {}, "NO_PROJECT", "No project open"};
            QString name = args["name"].toString();
            if (name.isEmpty()) return {false, {}, "MISSING_PARAM", "'name' required"};
            ProjectManager::instance().setDefaultLanguage(name);
            return {true, QJsonObject{{"success", true}, {"defaultLanguage", name}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_voice_language";
        def.displayName = "Set Voice Language";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"language", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            QString lang = args["language"].toString().trimmed();
            if (lang.isEmpty())
                return {false, {}, "INVALID_ARGS", "language is required"};
            const auto normalized = ProjectLanguageSemantics::normalize({lang});
            if (!normalized.ok())
                return {false, {}, "INVALID_ARGS", normalized.error};
            uint32_t langId = HarkenEditor::Bridge::packLanguageIdFromName(lang);
            if (engine)
                engine->setRuntimeLanguage(lang);
            return {true, QJsonObject{
                {"success", true}, {"language", lang},
                {"language_id", static_cast<int>(langId)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_runtime_language";
        def.displayName = "Set Runtime Language";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"code", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            QString code = args["code"].toString();
            if (code.isEmpty()) return {false, {}, "MISSING_PARAM", "'code' required"};
            if (engine)
                engine->setRuntimeLanguage(code);
            return {true, QJsonObject{{"success", true}, {"language", code}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "import_audio";
        def.displayName = "Import Audio";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"path", "string"}, {"pack", "string"}, {"target_parent_path", "string"}, {"target_parent_id", "uint32"}, {"as_music_source", "bool"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            if (!ProjectManager::instance().isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString path = args["path"].toString();
            QString packName = args["pack"].toString();
            QString targetParentPath = args["target_parent_path"].toString().trimmed();
            uint32_t targetParentID = static_cast<uint32_t>(args["target_parent_id"].toInteger(0));
            const bool asMusicSource = args["as_music_source"].toBool(false);

            if (path.isEmpty())
                return {false, {}, "HARKEN_ERR_MISSING_PARAM", "Audio file path is required"};

            ProjectData* project = ProjectManager::instance().currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            const bool hasExplicitTarget =
                !packName.isEmpty() || targetParentID != 0 || !targetParentPath.isEmpty();
            if (!hasExplicitTarget)
            {
                auto* cmd = ImportAssetsCommand::pushImportAtRoot({path});
                if (cmd->successCount() <= 0)
                    return {false, {}, "IMPORT_FAILED", "Failed to import audio file"};

                QList<uint32_t> ids = cmd->importedIDs();
                QList<QUuid> itemGuids = cmd->importedItemGuids();
                if (!ids.isEmpty()) {
                    uint32_t sourceID = ids.first();
                    AssetInfo* info = AssetRegistry::instance().getAsset(sourceID);
                    if (info) {
                        ProjectItem* rootItem = nullptr;
                        if (!itemGuids.isEmpty() && !itemGuids.first().isNull())
                            rootItem = ProjectManager::instance().findItemByGuid(itemGuids.first());
                        QJsonObject resp{
                            {"source_id", static_cast<qint64>(sourceID)},
                            {"relative_path", info->relativePath},
                            {"absolute_path", info->absolutePath},
                            {"sample_rate", static_cast<qint64>(info->metadata.sampleRate)},
                            {"channels", info->metadata.channels},
                            {"duration_sec", info->metadata.durationSec},
                            {"target_parent_id", static_cast<qint64>(rootItem ? itemShortID(rootItem) : 0)},
                            {"target_parent_authoring_handle", static_cast<qint64>(rootItem ? rootItem->objectID : 0)},
                            {"target_parent_name", rootItem ? rootItem->name : QString()},
                            {"is_music_source", asMusicSource}};
                        if (asMusicSource)
                            resp["music_source_id"] = static_cast<qint64>(sourceID);
                        return {true, resp};
                    }
                }
                return {true, QJsonObject{
                    {"success", true}, {"count", cmd->successCount()},
                    {"is_music_source", asMusicSource}}};
            }

            if (packName.isEmpty())
                return {false, {}, "HARKEN_ERR_MISSING_PARAM", "Pack name is required"};

            ProjectFileBinding* targetBinding = nullptr;
            const auto matches = project->findItemsBySelector(packName);
            if (matches.size() > 1)
                return {false, {}, "AMBIGUOUS_SELECTOR", "Multiple objects match this pack name; provide its GUID."};
            for (auto* binding : project->allFileBindings()) {
                if (matches.size() == 1 && binding && binding->rootItem.get() == matches.front()) {
                    targetBinding = binding;
                    break;
                }
            }
            if (!targetBinding)
                return {false, {}, "HARKEN_ERR_NOT_FOUND",
                    QString("Pack '%1' not found").arg(packName)};

            ProjectItem* targetParentByID = nullptr;
            ProjectItem* targetParentByPath = nullptr;
            if (targetParentID != 0) {
                targetParentByID = findItemByObjectIDInProject(project, targetParentID);
                if (!targetParentByID)
                    return {false, {}, "PARENT_NOT_FOUND",
                        QString("target_parent_id '%1' not found").arg(targetParentID)};
            }
            if (!targetParentPath.isEmpty()) {
                targetParentByPath = findItemByPathInProject(project, targetParentPath);
                if (!targetParentByPath)
                    return {false, {}, "PARENT_NOT_FOUND",
                        QString("target_parent_path '%1' not found").arg(targetParentPath)};
            }
            if (targetParentByID && targetParentByPath && targetParentByID != targetParentByPath)
                return {false, {}, "INVALID_ARGS",
                    "target_parent_id and target_parent_path point to different items"};

            ProjectItem* targetParent = targetBinding->rootItem ? targetBinding->rootItem.get() : nullptr;
            if (targetParentByID) targetParent = targetParentByID;
            else if (targetParentByPath) targetParent = targetParentByPath;
            if (!targetParent)
                return {false, {}, "PARENT_NOT_FOUND", "Target binding root is not available"};

            ProjectFileBinding* parentBinding = nullptr;
            if (ProjectData* curProject = ProjectManager::instance().currentProject())
            {
                parentBinding = findFileBindingForItem(*curProject, targetParent);
            }
            if (parentBinding != targetBinding)
                return {false, {}, "INVALID_PARENT",
                    "Import target parent must belong to the target Pack"};
            if (!ProjectManager::canAcceptImportedAudioParent(targetParent))
                return {false, {}, "INVALID_PARENT",
                    QString("Item '%1' cannot contain imported audio children").arg(targetParent->name)};

            QStringList files;
            files << path;
            int successCount = 0;
            QList<uint32_t> ids;
            if (targetParent->type == ProjectItem::Type::Sound) {
                auto* cmd = ImportSoundSourcesCommand::pushImport(
                    files, targetBinding, targetParent);
                successCount = cmd->successCount();
                ids = cmd->importedIDs();
            } else {
                auto* cmd = ImportAssetsCommand::pushImport(
                    files, targetBinding, targetParent);
                successCount = cmd->successCount();
                ids = cmd->importedIDs();
            }

            if (successCount > 0) {
                if (!ids.isEmpty()) {
                    uint32_t sourceID = ids.first();
                    AssetInfo* info = AssetRegistry::instance().getAsset(sourceID);
                    if (info) {
                        QJsonObject resp{
                            {"source_id", static_cast<qint64>(sourceID)},
                            {"relative_path", info->relativePath},
                            {"absolute_path", info->absolutePath},
                            {"sample_rate", static_cast<qint64>(info->metadata.sampleRate)},
                            {"channels", info->metadata.channels},
                            {"duration_sec", info->metadata.durationSec},
                            {"target_parent_id", static_cast<qint64>(itemShortID(targetParent))},
                            {"target_parent_authoring_handle", static_cast<qint64>(targetParent->objectID)},
                            {"target_parent_name", targetParent->name},
                            {"is_music_source", asMusicSource}};
                        if (asMusicSource) {
                            resp["music_source_id"] = static_cast<qint64>(sourceID);
                        }
                        return {true, resp};
                    }
                }
                return {true, QJsonObject{
                    {"success", true}, {"count", successCount},
                    {"target_parent_id", static_cast<qint64>(itemShortID(targetParent))},
                    {"target_parent_authoring_handle", static_cast<qint64>(targetParent->objectID)},
                    {"target_parent_name", targetParent->name},
                    {"is_music_source", asMusicSource}}};
            }
            return {false, {}, "IMPORT_FAILED", "Failed to import audio file"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "import_audio_to_canvas";
        def.displayName = "Import Audio To Blueprint Canvas";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"files", "array"}, {"drop_x", "float"}, {"drop_y", "float"}, {"target_container_id", "uint32"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            if (!ProjectManager::instance().isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QJsonArray filesArr = args["files"].toArray();
            const uint32_t targetContainerID =
                static_cast<uint32_t>(args["target_container_id"].toInteger(0));

            if (filesArr.isEmpty())
                return {false, {}, "MISSING_PARAM", "'files' array is required and non-empty"};

            QStringList files;
            files.reserve(filesArr.size());
            for (const QJsonValue& v : filesArr) {
                const QString p = v.toString();
                if (p.isEmpty()) continue;
                files.push_back(p);
            }
            files.removeDuplicates();
            if (files.isEmpty())
                return {false, {}, "MISSING_PARAM", "'files' contains no valid path strings"};

            ProjectData* project = ProjectManager::instance().currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            ProjectItem* dropContext = nullptr;
            if (targetContainerID != 0) {
                dropContext = findItemByObjectIDInProject(project, targetContainerID);
                if (!dropContext)
                    return {false, {}, "PARENT_NOT_FOUND",
                        QString("target_container_id '%1' not found").arg(targetContainerID)};
            }

            const auto target = Blueprint::resolveAudioDropTarget(dropContext);
            if (!target.ok())
                return {false, {}, "INVALID_PARENT", target.error};

            using Kind = Blueprint::AudioDropKind;
            ProjectItem* targetParent = target.kind == Kind::Parent ? target.item : nullptr;
            ProjectItem* targetSound = target.kind == Kind::Sources ? target.item : nullptr;
            ProjectItem* targetBus = target.kind == Kind::RootOutput ? target.item : nullptr;
            int successCount = 0;
            int failCount = 0;
            int duplicateCount = 0;
            QList<uint32_t> importedSources;
            QList<QUuid> importedGuids;
            if (targetSound) {
                auto* command = ImportSoundSourcesCommand::pushImport(files,
                    findFileBindingForItem(*project, targetSound), targetSound, target.language);
                successCount = command->successCount();
                failCount = command->failCount();
                duplicateCount = command->duplicateCount();
                importedSources = command->importedIDs();
            } else {
                auto* command = targetParent
                    ? ImportAssetsCommand::pushImport(files,
                        findFileBindingForItem(*project, targetParent), targetParent)
                    : ImportAssetsCommand::pushImportAtRoot(files,
                        targetBus ? targetBus->guid : QUuid{});
                successCount = command->successCount();
                failCount = command->failCount();
                duplicateCount = command->duplicateCount();
                importedSources = command->importedIDs();
                importedGuids = command->importedItemGuids();
            }
            QJsonArray createdNodeIds;
            QJsonArray createdHandles;
            QJsonArray createdGuids;
            QJsonArray sourceIds;
            for (uint32_t sourceID : importedSources)
                sourceIds.append(static_cast<qint64>(sourceID));
            for (const QUuid& guid : importedGuids) {
                if (auto* child = ProjectManager::instance().findItemByGuid(guid)) {
                    createdNodeIds.append(static_cast<qint64>(itemShortID(child)));
                    createdHandles.append(static_cast<qint64>(child->objectID));
                    createdGuids.append(guid.toString(QUuid::WithoutBraces));
                }
            }

            const QJsonObject data{
                {"success", successCount > 0},
                {"scenario", targetSound ? "sources" : targetParent ? "into_container"
                    : targetBus ? "root_output" : "root"},
                {"success_count", successCount},
                {"fail_count", failCount},
                {"duplicate_count", duplicateCount},
                {"source_ids", sourceIds},
                {"created_node_ids", createdNodeIds},
                {"created_authoring_handles", createdHandles},
                {"created_node_guids", createdGuids},
                {"container_id", 0},
                {"target_parent_id", targetParent ? static_cast<qint64>(itemShortID(targetParent)) : 0},
                {"target_parent_authoring_handle", targetParent ? static_cast<qint64>(targetParent->objectID) : 0},
                {"target_sound_authoring_handle", targetSound ? static_cast<qint64>(targetSound->objectID) : 0},
                {"target_bus_authoring_handle", targetBus ? static_cast<qint64>(targetBus->objectID) : 0}
            };
            if (successCount == 0)
                return {false, data, "IMPORT_FAILED", "No audio file was imported"};
            return {true, data};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_conversion_settings";
        def.displayName = "Set Conversion Settings";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"source_id", "uint32"}, {"codec", "string"}, {"quality", "float"}, {"resample", "bool"}, {"target_sample_rate", "uint32"}, {"as_override", "bool"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            uint32_t sourceID = static_cast<uint32_t>(args["source_id"].toInteger());
            if (sourceID == 0)
                return {false, {}, "HARKEN_ERR_MISSING_PARAM", "source_id is required"};
            AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
            if (!asset)
                return {false, {}, "HARKEN_ERR_NOT_FOUND",
                    QString("Asset %1 not found").arg(sourceID)};
            ConversionSettings settings = asset->settings;
            const uint32_t oldProcessingHash = settings.hash();
            if (args.contains("codec"))
                settings.format = ConversionSettings::codecFromString(args["codec"].toString());
            if (args.contains("quality"))
                settings.vorbisQuality = static_cast<float>(args["quality"].toDouble());
            if (args.contains("resample"))
                settings.performResample = args["resample"].toBool();
            if (args.contains("target_sample_rate"))
                settings.targetSampleRate = static_cast<uint32_t>(args["target_sample_rate"].toInteger());
            if (args.contains("force_mono") || args.contains("trim_silence") || args.contains("trim_threshold_db")
                || args.contains("normalize") || args.contains("normalize_target_db"))
                return {false, {}, "INVALID_ARGS", "Mono conversion, silence trimming and normalization settings are no longer supported"};
            asset->settings = settings;
            bool asOverride = args["as_override"].toBool(false);
            if (asOverride)
                PlatformProfileManager::instance().setFileOverride(sourceID, settings);
            if (settings.hash() != oldProcessingHash)
                AssetRegistry::instance().invalidateAllCaches(sourceID);
            return {true, QJsonObject{
                {"source_id", static_cast<qint64>(sourceID)},
                {"settings", settings.toJson()}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_platform_profile";
        def.displayName = "Set Platform Profile";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"platform", "string"}, {"default_config", "object"}};
        def.undoable = false;
        def.handler = [](const QJsonObject&) -> ActionResult {
            return {false, {}, "PLATFORM_INSTANCES_REQUIRED",
                    "Use set_project_platform_profile with platform_id and profile; global platform profiles are no longer editable"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "cleanup_cache";
        def.displayName = "Cleanup Cache";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"max_age_days", "int"}, {"max_size_bytes", "int64"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            int maxAgeDays = args["max_age_days"].toInt(90);
            qint64 maxSizeBytes = args["max_size_bytes"].toInteger(0);
            AssetGarbageCollector tempGC;
            tempGC.setProjectRoot(AssetRegistry::instance().projectRoot());
            CleanupResult result = tempGC.cleanupLRU(maxAgeDays, static_cast<uint64_t>(maxSizeBytes));
            return {true, QJsonObject{
                {"deleted_count", result.deletedCount},
                {"freed_bytes", static_cast<qint64>(result.freedBytes)},
                {"deleted_files", QJsonArray::fromStringList(result.deletedFiles)}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "rescan_media_sources";
        def.displayName = "Rescan Media Sources";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"platform_id", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto id = ProjectPlatformSemantics::parseId(args["platform_id"].toString());
            const auto* project = ProjectManager::instance().currentProject();
            if (args.contains("platform") || !id || !project
                || !ProjectPlatformSemantics::find(project->platforms, *id))
                return {false, {}, "INVALID_PLATFORM", "platform_id must identify a platform in the current project"};
            const auto platform = *id;
            const int queued = MediaPool::instance().rescan(platform);
            return {true, QJsonObject{
                {"queued", queued},
                {"platform_id", ProjectPlatformSemantics::idString(platform)},
                {"message", QStringLiteral("Media source status rescan queued.")}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "batch_replace_media";
        def.displayName = "Batch Replace Media";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"replacements", "object"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            QMap<uint32_t, QString> replacements = replacementMapFromArgs(args);
            if (replacements.isEmpty())
                return {false, {}, "HARKEN_ERR_MISSING_PARAM", "replacements object is required"};
            auto results = MediaPool::instance().batchReplace(replacements);
            QJsonArray resultsArr;
            int succeeded = 0, failed = 0;
            for (const auto& r : results) {
                QJsonObject entry;
                entry["source_id"] = static_cast<qint64>(r.sourceID);
                entry["sourceID"] = static_cast<qint64>(r.sourceID);
                entry["success"] = r.success;
                if (!r.success) entry["error"] = r.errorMessage;
                resultsArr.append(entry);
                if (r.success) ++succeeded; else ++failed;
            }
            return {true, QJsonObject{
                {"results", resultsArr}, {"succeeded", succeeded}, {"failed", failed}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "batch_convert_media";
        def.displayName = "Batch Convert Media";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"source_ids", "array"}, {"platform_id", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            QJsonArray sourceIDsArr = sourceIDArrayFromArgs(args);
            if (sourceIDsArr.isEmpty())
                return {false, {}, "HARKEN_ERR_MISSING_PARAM", "source_ids array is required"};
            const auto id = ProjectPlatformSemantics::parseId(args["platform_id"].toString());
            const auto* project = ProjectManager::instance().currentProject();
            if (args.contains("platform") || !id || !project
                || !ProjectPlatformSemantics::find(project->platforms, *id))
                return {false, {}, "INVALID_PLATFORM", "platform_id must identify a platform in the current project"};
            const auto platform = *id;
            QVector<uint32_t> sourceIDs;
            sourceIDs.reserve(sourceIDsArr.size());
            for (const auto& val : sourceIDsArr) {
                uint32_t id = static_cast<uint32_t>(val.toInteger());
                if (id != 0) sourceIDs.append(id);
            }
            MediaPool::instance().batchConvert(sourceIDs, platform);
            return {true, QJsonObject{
                {"queued", sourceIDs.size()},
                {"platform_id", ProjectPlatformSemantics::idString(platform)},
                {"message", QStringLiteral("Batch conversion started. Monitor via get_media_status.")}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_buffer_size";
        def.displayName = "Set Buffer Size";
        def.category = "asset";
        def.paramSchema = QJsonObject{{"block_size", "int"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return {false, {}, "ENGINE_NOT_AVAILABLE", "AudioEngine not available"};
            int blockSize = args["block_size"].toInt(0);
            if (blockSize < 64 || blockSize > 4096 || (blockSize & (blockSize - 1)) != 0)
                return {false, {}, "INVALID_PARAM",
                    "block_size must be power of 2 between 64 and 4096"};
            bool ok = engine->reconfigureBufferSize(blockSize);
            if (!ok) return {false, {}, "RECONFIGURE_FAILED", "Buffer resize failed"};
            return {true, QJsonObject{{"block_size", blockSize}}};
        };
        mgr.registerAction(def);
    }
}

}
