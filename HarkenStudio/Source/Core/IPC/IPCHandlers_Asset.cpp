#include "../PropertyEditSession.h"
#include "../ProjectBuildSnapshot.h"
#include "IPCServerInternal.h"
#include "AudioEngine.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/WaveformCache.h"
#include "PackGenerationRecord.h"
#include "PackArtifactCache.h"
#include "ProjectPlatformActivity.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <memory>
#include <map>
#include <tuple>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

namespace {

QJsonArray sourceIDArrayFromArgs(const QJsonObject& args)
{
    QJsonArray arr = args.value(QStringLiteral("source_ids")).toArray();
    if (arr.isEmpty()) {
        arr = args.value(QStringLiteral("sourceIDs")).toArray();
    }
    return arr;
}

QMap<uint32_t, QString> replacementMapFromArgs(const QJsonObject& args)
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

template <typename T>
bool readPod(const QByteArray& bytes, qsizetype offset, T& out)
{
    if (offset < 0 || offset + static_cast<qsizetype>(sizeof(T)) > bytes.size())
        return false;
    std::memcpy(&out, bytes.constData() + offset, sizeof(T));
    return true;
}

QJsonObject packCodecFlagsJson(uint32_t flags)
{
    const Bridge::PackCodecFlagsView v = Bridge::decodePackCodecFlags(flags);
    return QJsonObject{
        {QStringLiteral("fullDecodeFallback"), v.fullDecodeFallback},
        {QStringLiteral("compressedResident"), v.compressedResident},
        {QStringLiteral("hasPacketTable"), v.hasPacketTable},
        {QStringLiteral("hasSeekTable"), v.hasSeekTable},
        {QStringLiteral("hasSharedCodebook"), v.hasSharedCodebook},
        {QStringLiteral("loopEnabled"), v.loopEnabled},
        {QStringLiteral("vorbisVbrQuality"), v.vorbisVbrQuality},
        {QStringLiteral("vorbisManagedBitrate"), v.vorbisManagedBitrate}
    };
}

QJsonArray inspectPackCodecMetadataSection(const QByteArray& payload, QString* error)
{
    QJsonArray entries;
    std::vector<Bridge::PackCodecEntryInfo> infos;
    Bridge::parsePackCodecMetadataSection(payload, infos, error);

    for (const auto& entry : infos) {
        entries.append(QJsonObject{
            {QStringLiteral("sourceID"), static_cast<qint64>(entry.sourceId)},
            {QStringLiteral("codecID"), static_cast<int>(entry.codecId)},
            {QStringLiteral("codecName"), Bridge::packCodecName(entry.codecId)},
            {QStringLiteral("containerID"), static_cast<int>(entry.containerId)},
            {QStringLiteral("containerName"), Bridge::packCodecContainerName(entry.containerId)},
            {QStringLiteral("channels"), static_cast<int>(entry.channels)},
            {QStringLiteral("sampleRate"), static_cast<int>(entry.sampleRate)},
            {QStringLiteral("channelLayoutMask"), QStringLiteral("0x%1").arg(entry.channelLayoutMask, 8, 16, QChar('0'))},
            {QStringLiteral("frameCount"), static_cast<double>(entry.frameCount)},
            {QStringLiteral("loopStartFrame"), static_cast<double>(entry.loopStartFrame)},
            {QStringLiteral("loopEndFrame"), static_cast<double>(entry.loopEndFrame)},
            {QStringLiteral("packetTableOffset"), static_cast<int>(entry.packetTableOffset)},
            {QStringLiteral("packetTableCount"), static_cast<int>(entry.packetTableCount)},
            {QStringLiteral("seekTableOffset"), static_cast<int>(entry.seekTableOffset)},
            {QStringLiteral("seekTableCount"), static_cast<int>(entry.seekTableCount)},
            {QStringLiteral("codebookID"), static_cast<qint64>(entry.codebookId)},
            {QStringLiteral("codebookHash"), QStringLiteral("0x%1").arg(entry.codebookHash, 8, 16, QChar('0'))},
            {QStringLiteral("decodeAllocBytes"), static_cast<int>(entry.decodeAllocBytes)},
            {QStringLiteral("flags"), packCodecFlagsJson(entry.flags)}
        });
    }
    return entries;
}

QJsonArray inspectPackCodecCodebookSection(const QByteArray& payload, QString* error)
{
    QJsonArray entries;
    std::vector<Bridge::PackCodecCodebookInfo> infos;
    Bridge::parsePackCodecCodebookSection(payload, infos, error);

    for (const auto& entry : infos) {
        entries.append(QJsonObject{
            {QStringLiteral("codebookID"), static_cast<qint64>(entry.codebookId)},
            {QStringLiteral("codebookHash"), QStringLiteral("0x%1").arg(entry.codebookHash, 8, 16, QChar('0'))},
            {QStringLiteral("offset"), static_cast<int>(entry.offset)},
            {QStringLiteral("size"), static_cast<int>(entry.size)}
        });
    }
    return entries;
}

}

void IPCServer::registerAssetHandlers()
{
    commandHandlers_["export_pack"] = [this](const QJsonObject& args) { return handleExportPack(args); };
    commandHandlers_["get_pack_build_status"] = [this](const QJsonObject& args) { return handleGetPackBuildStatus(args); };
    commandHandlers_["cancel_pack_build"] = [this](const QJsonObject& args) { return handleCancelPackBuild(args); };
    commandHandlers_["get_pack_info"] = [this](const QJsonObject& args) { return handleGetPackInfo(args); };
    commandHandlers_["validate_pack"] = [this](const QJsonObject& args) { return handleValidatePack(args); };
    commandHandlers_["get_pack_dependencies"] = [this](const QJsonObject& args) { return handleGetPackDependencies(args); };
    commandHandlers_["get_unused_assets"] = [this](const QJsonObject& args) { return handleGetUnusedAssets(args); };

    commandHandlers_["get_available_languages"] = [this](const QJsonObject& args) {
        Q_UNUSED(args);
        QJsonArray arr;
        arr.append("English");
        arr.append("Japanese");
        arr.append("Chinese(Simplified)");
        arr.append("Chinese(Traditional)");
        arr.append("Korean");
        arr.append("French");
        arr.append("German");
        arr.append("Spanish");
        arr.append("Italian");
        arr.append("Portuguese");
        arr.append("Russian");
        return successResponse(QJsonObject{{"languages", arr}});
    };

    commandHandlers_["list_project_languages"] = [this](const QJsonObject&) -> QJsonObject {
        if (!ProjectManager::instance().isProjectOpen())
            return errorResponse("NO_PROJECT", "No project open");
        QJsonArray arr;
        for (const auto& lang : ProjectManager::instance().getProjectLanguages())
            arr.append(lang);
        QJsonObject r; r["success"] = true;
        r["languages"] = arr;
        r["defaultLanguage"] = ProjectManager::instance().getDefaultLanguage();
        return r;
    };

    commandHandlers_["import_audio"] = [this](const QJsonObject& args) { return handleImportAudio(args); };
    commandHandlers_["get_transcode_status"] = [this](const QJsonObject& args) {
        const auto id = ProjectPlatformSemantics::parseId(args.value("platform_id").toString());
        if (args.keys() != QStringList{"platform_id", "source_id"} || !id
            || args.value("source_id").toInteger() <= 0)
            return errorResponse("INVALID_ARGS", "Provide platform_id and source_id from transcode_asset");
        if (!transcodeRequest_ || transcodeRequest_->platform.id != *id
            || transcodeRequest_->asset.sourceID != args.value("source_id").toInteger())
            return errorResponse("CONVERSION_NOT_FOUND", "No conversion requested for this platform and source");
        if (!AssetRegistry::instance().accepts(*transcodeRequest_))
            return errorResponse("STALE_CONVERSION", "The project, platform or asset changed; request a new conversion");
        return successResponse({{"in_progress", transcodeInProgress_},
            {"platform_id", args.value("platform_id")}, {"source_id", args.value("source_id")},
            {"result", transcodeResult_}});
    };
    commandHandlers_["transcode_asset"] = [this](const QJsonObject& args) { return handleTranscodeAsset(args); };
    commandHandlers_["get_asset_info"] = [this](const QJsonObject& args) { return handleGetAssetInfo(args); };
    commandHandlers_["list_assets"] = [this](const QJsonObject& args) { return handleListAssets(args); };

    commandHandlers_["get_conversion_settings"] = [this](const QJsonObject& args) { return handleGetConversionSettings(args); };
    commandHandlers_["get_platform_profile"] = [this](const QJsonObject& args) { return handleGetPlatformProfile(args); };

    commandHandlers_["get_media_status"] = [this](const QJsonObject& args) { return handleGetMediaStatus(args); };
    commandHandlers_["rescan_media_sources"] = [this](const QJsonObject& args) { return handleRescanMediaSources(args); };

    commandHandlers_["regenerate_waveform_cache"] = [this](const QJsonObject& args) -> QJsonObject {
        QString path = args["path"].toString();
        if (!path.isEmpty()) {
            bool ok = WaveformCache::generate(path);
            return ok
                ? successResponse(QJsonObject{{"path", path}, {"regenerated", true}})
                : errorResponse("GENERATE_FAILED", "Failed to regenerate VKD for: " + path);
        }

        if (!projectManager_ || !projectManager_->isProjectOpen())
            return errorResponse("NO_PROJECT", "No project open");

        ProjectData* project = projectManager_->currentProject();
        QString projectDir = QFileInfo(project->projectPath).absolutePath();
        QString AudioSourcesDir = QDir(projectDir).filePath(QStringLiteral("AudioSources"));
        auto future = WaveformCache::rebuildAllAsync(project, AudioSourcesDir);
        future.waitForFinished();
        int rebuilt = future.result();

        return successResponse(QJsonObject{
            {"rebuilt", rebuilt},
            {"message", QStringLiteral("Rebuilt %1 VKD files").arg(rebuilt)}
        });
    };


    commandHandlers_["get_pack_export_settings"] = [this](const QJsonObject&) -> QJsonObject {
        if (!projectManager_ || !projectManager_->isProjectOpen())
            return errorResponse("NO_PROJECT_OPEN", "No project is currently open");

        ProjectData* project = projectManager_->currentProject();
        if (!project)
            return errorResponse("NO_PROJECT_OPEN", "No project is currently open");

        auto effective = [](const QString& v, const QString& def) {
            return v.isEmpty() ? def : v;
        };

        QJsonObject result{
            {"pack_build_output_dir", effective(project->packBuildOutputDir,
                                                 PathUtils::buildPacksDirName())},
            {"pack_build_output_root", PathUtils::packBuildOutputRoot(
                 project->projectPath, project->packBuildOutputDir)},
            {"pack_file_extension", Bridge::packArtifactExtension()}
        };
        return successResponse(result);
    };

    commandHandlers_["validate_pack_build_readiness"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!projectManager_ || !projectManager_->isProjectOpen())
            return errorResponse("NO_PROJECT_OPEN", "No project is currently open");
        ProjectData* project = projectManager_->currentProject();
        if (!project)
            return errorResponse("NO_PROJECT_OPEN", "No project is currently open");

        Q_UNUSED(args);
        const QString outputDir = PathUtils::packBuildOutputRoot(
            project->projectPath, project->packBuildOutputDir);
        const auto vr = ProjectPackExporter::ValidatePackBuildReadiness(
            *project, outputDir);

        QJsonObject result;
        result["valid"]        = vr.valid;
        result["errorCount"]   = vr.errorCount;
        result["warningCount"] = vr.warningCount;
        QJsonArray issues;
        for (const auto& issue : vr.issues) {
            QJsonObject obj;
            obj["severity"] = (issue.severity == ProjectPackExporter::ValidationIssue::Error) ? "error"
                            : (issue.severity == ProjectPackExporter::ValidationIssue::Warning) ? "warning"
                            : "info";
            obj["message"] = issue.message;
            if (issue.objectID != 0)
                obj["objectID"] = QString("0x%1").arg(issue.objectID, 8, 16, QChar('0'));
            issues.append(obj);
        }
        result["issues"] = issues;
        return successResponse(result);
    };

}


QJsonObject IPCServer::handleExportPack(const QJsonObject& args)
{
    if (!projectManager_)
    {
        return errorResponse("NO_PROJECT_OPEN",
                             "No project is currently open");
    }

    const auto project = projectManager_->currentProjectLease();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    QString outputPath = args["output_path"].toString().trimmed();
    if (outputPath.isEmpty())
        outputPath = PathUtils::packBuildOutputRoot(
            project->projectPath, project->packBuildOutputDir);
    else
        outputPath = PathUtils::toAbsolutePath(
            outputPath, QFileInfo(project->projectPath).absolutePath());
    const QFileInfo outputInfo(outputPath);
    if (PathUtils::isProtectedPackOutputPath(outputPath, project->projectPath))
        return errorResponse("INVALID_ARGS", PathUtils::packBuildOutputLocationError());
    if (outputInfo.isFile()
        || (!args["output_path"].toString().trimmed().isEmpty()
            && (outputInfo.fileName().endsWith(Bridge::packArtifactExtension(), Qt::CaseInsensitive)
                || outputInfo.fileName().endsWith(QStringLiteral(".pack"), Qt::CaseInsensitive))))
    {
        return errorResponse("INVALID_ARGS",
                             "output_path must be an output directory; single pack files are not supported");
    }

    if (packBuildInProgress_ || PackGenerationRecordStore::instance().isExporting())
    {
        return errorResponse("PACK_BUILD_IN_PROGRESS",
                             "A pack build is already running. Poll get_pack_build_status.");
    }

    const QString actionName = args["action_name"].toString().trimmed();
    harkenstudio::ObjectId actionStableId = {};
    if (!actionName.isEmpty()) {
        const auto units = ProjectPackExporter::CollectActionPackUnits(*project);
        for (const auto& unit : units) {
            if (unit.name != actionName)
                continue;
            if (!harkenstudio::IsNull(actionStableId)) {
                return errorResponse("INVALID_ARGS",
                                     "action_name is ambiguous; use a unique Action name: " + actionName);
            }
            actionStableId = unit.stableId;
        }
        if (harkenstudio::IsNull(actionStableId)) {
            return errorResponse("INVALID_ARGS",
                                 "action_name does not identify an Action: " + actionName);
        }
    }

    if (args.contains("platform") || args.contains("platforms")
        || (args.contains("platform_id") && !ProjectPlatformSemantics::parseId(args.value("platform_id").toString())))
        return errorResponse("INVALID_PLATFORM", "Use platform_id with a project platform UUID, or omit it for the export selection");
    const QString explicitPlatform = args["platform_id"].toString();
    const QString language = args.value(QStringLiteral("language")).toString();
    const auto exportSelection = projectManager_->packExportSelection();
    QString platformError;
    const QStringList platforms = ProjectPackExporter::resolveExportPlatforms(
        *project, explicitPlatform, exportSelection, &platformError);
    if (platforms.isEmpty())
    {
        return errorResponse(explicitPlatform.isEmpty() ? "NO_EXPORT_PLATFORMS"
                                                        : "INVALID_PLATFORM",
                             platformError);
    }
    PropertyEditSession::finishAllActive(true);
    QString snapshotError;
    const auto snapshot = ProjectBuildSnapshot::capture(*project, &snapshotError);
    if (!snapshot) return errorResponse("INVALID_PROJECT", snapshotError);
    QString outputLockError;
    const auto outputLock = ProjectPackExporter::LockPackOutputRoot(outputPath, &outputLockError);
    if (!outputLock) return errorResponse("OUTPUT_BUSY", outputLockError);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const quint64 runId = PackGenerationRecordStore::instance().beginProgress(cancel);
    if (runId == 0)
        return errorResponse("PACK_BUILD_IN_PROGRESS",
                             "A pack build is already running. Poll get_pack_build_status.");
    const auto platformChanged = std::make_shared<std::atomic_bool>(false);
    const auto platformObserver = std::shared_ptr<QObject>(new QObject,
        [](QObject* observer) { observer->deleteLater(); });
    connect(projectManager_, &ProjectManager::targetPlatformsChanged, platformObserver.get(),
        [platformChanged, snapshot, project, platforms] {
            platformChanged->store(!ProjectPackExporter::ValidatePackBuildPlatforms(
                snapshot->project.platforms, project->platforms, platforms), std::memory_order_relaxed);
        });
    packBuildProject_ = project;
    packBuildInProgress_ = true;
    packBuildProgress_ = 0;
    packBuildMessage_ = "Checking existing packs and output permissions...";
    packBuildResult_ = QJsonObject();
    packBuildCancel_ = cancel;

    QPointer<ProjectManager> pmGuard = projectManager_;
    ProjectPackExporter::beginBackgroundExport();
    const auto platformActivity = ProjectPlatformActivity::acquire();
    QThread* buildThread = QThread::create(
        [this, pmGuard, project, snapshot, outputPath, platforms, actionName,
         actionStableId, language, exportSelection, cancel, runId, platformActivity, outputLock,
         platformChanged, platformObserver]() {
        struct ExportGate {
            ~ExportGate() { ProjectPackExporter::endBackgroundExport(); }
        } gate;
        ProjectPackExporter exporter;

        QJsonArray platformResults;
        QVector<PackGenerationRecord> generationRecords;
        std::map<std::tuple<QString, QString, QString>, PackGenerationProgress> unitProgress;
        QMap<QString, QVector<PackArtifactMetadata>> publishedArtifacts;
        bool allSuccess = true;
        QString firstError;
        int done = 0;
        QStringList succeededPlatforms;
        ProjectPackExporter::ExportOptions exportOpts;
        exportOpts.snapshot = snapshot;
        exportOpts.selection = exportSelection;
        exportOpts.language = language;
        if (!harkenstudio::IsNull(actionStableId))
            exportOpts.onlyActionStableIds.insert(actionStableId);
        exportOpts.cancelCheck = [cancel]() {
            return cancel->load(std::memory_order_relaxed);
        };
        exportOpts.progressCallback = [this, pmGuard, project, runId, &unitProgress](const PackGenerationProgress& progress) {
            unitProgress[{progress.platform, progress.language, progress.unitKey}] = progress;
            QMetaObject::invokeMethod(this, [pmGuard, project, runId, progress]() {
                if (!pmGuard || pmGuard->currentProject() != project.get()) return;
                PackGenerationRecordStore::instance().updateProgress(runId, progress);
            }, Qt::QueuedConnection);
        };
        exportOpts.debugStrings = true;
        const bool preflightSucceeded = ProjectPackExporter::ValidatePackBuildWriteAccess(
            snapshot->project, outputPath, platforms, exportOpts, &firstError);
        allSuccess = preflightSucceeded;
        if (preflightSucceeded) {
            QMetaObject::invokeMethod(this, [this, runId] {
                auto& store = PackGenerationRecordStore::instance();
                if (!store.isCurrentRun(runId)) return;
                store.finishPreflight(runId);
                packBuildMessage_ = QStringLiteral("Building packs...");
            }, Qt::QueuedConnection);
        }
        for (const QString& platform : platforms)
        {
            if (!preflightSucceeded || cancel->load(std::memory_order_relaxed)) break;
            exportOpts.platform = platform;

            QElapsedTimer timer;
            timer.start();
            const auto& definition = *ProjectPlatformSemantics::find(snapshot->project.platforms, QUuid(platform));
            const QString platformDir = ProjectPackExporter::platformOutputDir(outputPath, definition);
            std::vector<ProjectPackExporter::PackBuildRecord> records;
            const auto result = exporter.ExportAllPacks(
                snapshot->project, platformDir, exportOpts, &records);
            if (result.success) publishedArtifacts.insert(platform, result.artifacts);
            for (const auto& record : records) {
                QString source;
                switch (record.source) {
                case ProjectPackExporter::PackBuildRecord::Source::Group:
                    source = QStringLiteral("Group");
                    break;
                case ProjectPackExporter::PackBuildRecord::Source::Action:
                    source = QStringLiteral("Action");
                    break;
                case ProjectPackExporter::PackBuildRecord::Source::Init:
                    source = QStringLiteral("Init");
                    break;
                case ProjectPackExporter::PackBuildRecord::Source::Shared:
                    source = QStringLiteral("Shared");
                    break;
                }
                generationRecords.push_back({
                    record.packName, record.objectID, source, record.platform,
                    record.language,
                    record.success,
                    record.omitted ? QStringLiteral("Skipped")
                        : result.canceled && !record.success ? QStringLiteral("Canceled")
                        : record.success && !record.warnings.isEmpty()
                            ? (record.skipped ? QStringLiteral("Up to date with warnings") : QStringLiteral("Built with warnings"))
                        : record.success ? (record.skipped ? QStringLiteral("Up to date") : QStringLiteral("Built"))
                                         : QStringLiteral("Failed"),
                    record.outputPath, record.warnings.join(QLatin1Char('\n')),
                    record.error, record.fileSize, record.elapsedMs, record.platformName,
                    record.basePlatform, record.platformDefinition, record.unitKey, result.canceled && !record.success});
            }
            if (!result.success
                && std::none_of(records.begin(), records.end(), [](const auto& record) {
                    return !record.success;
                })) {
                generationRecords.push_back({
                    actionName.isEmpty() ? QStringLiteral("Pack export") : actionName,
                    0u, actionName.isEmpty() ? QStringLiteral("Export") : QStringLiteral("Action"),
                    platform, exportOpts.language, false,
                    result.canceled ? QStringLiteral("Canceled") : QStringLiteral("Failed"),
                    platformDir, result.warnings.join(QLatin1Char('\n')),
                    result.error, result.fileSize, timer.elapsed(), definition.name,
                    definition.basePlatform, definition.toJson(), {}, result.canceled});
            }

            if (result.success) succeededPlatforms.append(platform);
            allSuccess = allSuccess && result.success;
            if (!result.success && firstError.isEmpty())
                firstError = result.error;

            QJsonObject entry{
                {"platform_id", platform},
                {"platform_name", definition.name},
                {"base_platform", ProjectPlatformSemantics::basePlatformName(definition.basePlatform)},
                {"path", result.outputPath},
                {"success", result.success},
                {"file_size", result.fileSize},
                {"object_count", result.objectCount},
                {"media_count", result.mediaCount},
                {"media_cache_hits", result.mediaCacheHits},
                {"media_conversions", result.mediaConversions},
                {"media_fallbacks", result.mediaFallbacks},
                {"warnings", QJsonArray::fromStringList(result.warnings)}
            };
            if (!result.success)
                entry["error"] = result.error;
            platformResults.append(entry);

            ++done;
            const int percent = done * 100 / static_cast<int>(platforms.size());
            const QString message = (result.success ? QStringLiteral("Exported %1 (%2/%3)")
                : result.canceled ? QStringLiteral("Canceled %1 (%2/%3)") : QStringLiteral("Failed %1 (%2/%3)"))
                .arg(definition.name).arg(done).arg(platforms.size());
            QMetaObject::invokeMethod(this, [this, cancel, percent, message]() {
                if (cancel != packBuildCancel_ || cancel->load(std::memory_order_relaxed)) return;
                packBuildProgress_ = percent;
                packBuildMessage_ = message;
            }, Qt::QueuedConnection);
        }

        bool canceled = cancel->load(std::memory_order_relaxed);
        QString publicationError;
        QStringList publicationWarnings;
        QVector<PackArtifactMetadata> completedArtifacts;
        for (const auto& artifacts : publishedArtifacts)
            completedArtifacts.append(artifacts);
        const auto publicationCanceled = [cancel, platformChanged] {
            return cancel->load(std::memory_order_relaxed) || platformChanged->load(std::memory_order_relaxed);
        };
        if (preflightSucceeded && !canceled) {
            bool finalized;
            if (allSuccess) {
                const auto publication = ProjectPackExporter::FinalizePackExport(
                    outputPath, snapshot->project, platforms, false, completedArtifacts, publicationCanceled);
                publicationWarnings = publication.warnings;
                finalized = publication.success;
                canceled = publication.canceled && cancel->load(std::memory_order_relaxed);
                publicationError = publication.canceled && !canceled
                    ? QStringLiteral("Pack platforms changed during the build; output finalization stopped") : publication.error;
            } else {
                finalized = ProjectPackExporter::FinalizePackOutputs(outputPath, snapshot->project.platforms,
                    platforms, succeededPlatforms, &publicationError, completedArtifacts, publicationCanceled);
                canceled = !finalized && cancel->load(std::memory_order_relaxed);
                if (!finalized && platformChanged->load(std::memory_order_relaxed))
                    publicationError = QStringLiteral("Pack platforms changed during the build; output finalization stopped");
            }
            if (!finalized && !canceled) {
                allSuccess = false;
                firstError = publicationError;
            }
        }
        packBuildCompletion_ = [this, platformResults, generationRecords, unitProgress, publishedArtifacts,
                                         allSuccess, firstError, pmGuard, preflightSucceeded,
                                          canceled, runId, project, snapshot, platformActivity,
                                          succeededPlatforms, outputLock, actionStableId, platformObserver,
                                          publicationWarnings]() mutable {
            if (!pmGuard || !pmGuard->currentProject()
                || pmGuard->currentProject() != project.get()
                || !PackGenerationRecordStore::instance().isCurrentRun(runId)) {
                PackGenerationRecordStore::instance().finishProgress(runId, true);
                packBuildInProgress_ = false;
                packBuildProgress_ = 0;
                packBuildMessage_ = QStringLiteral("Export result discarded because the project changed");
                packBuildResult_ = QJsonObject();
                packBuildCancel_.reset();
                return;
            }
            QString publicationError;
            const bool finalized = !preflightSucceeded || ProjectPackExporter::ValidatePackBuildPlatforms(snapshot->project.platforms,
                project->platforms, succeededPlatforms, &publicationError);
            if (!finalized) {
                allSuccess = false;
                firstError = publicationError;
            }
            for (auto it = publishedArtifacts.cbegin(); it != publishedArtifacts.cend(); ++it)
                PackArtifactCache::instance().publish(it.key(), it.value(), !harkenstudio::IsNull(actionStableId));
            for (const auto& [key, progress] : unitProgress) {
                PackGenerationRecordStore::instance().updateProgress(runId,
                    {progress.platform, progress.language, progress.unitKey, 0, PackGenerationStage::Queued});
                PackGenerationRecordStore::instance().updateProgress(runId, progress);
            }
            for (const auto& record : generationRecords)
                PackGenerationRecordStore::instance().add(record);
            PackGenerationRecordStore::instance().finishProgress(
                runId, canceled);
            if (canceled) {
                packBuildInProgress_ = false;
                packBuildProgress_ = 0;
                packBuildMessage_ = QStringLiteral("Build cancelled by user");
                packBuildResult_ = QJsonObject();
                packBuildCancel_.reset();
                return;
            }
            if (!preflightSucceeded || (platformResults.size() == 1 && !allSuccess))
            {
                packBuildResult_ = QJsonObject{
                    {"status", "error"},
                    {"code", preflightSucceeded ? "EXPORT_PACK_FAILED" : "PACK_BUILD_PREFLIGHT_FAILED"},
                    {"message", firstError}
                };
            }
            else
            {
                QJsonObject resultObj{
                    {"success", allSuccess},
                    {"platforms", platformResults}
                };
                if (platformResults.size() == 1)
                {
                    const QJsonObject single = platformResults.first().toObject();
                    for (auto it = single.begin(); it != single.end(); ++it)
                        resultObj[it.key()] = it.value();
                }
                if (!publicationWarnings.isEmpty()) {
                    QJsonArray warnings = resultObj.value("warnings").toArray();
                    for (const QString& warning : publicationWarnings) warnings.append(warning);
                    resultObj["warnings"] = warnings;
                }
                packBuildResult_ = QJsonObject{
                    {"status", "ok"},
                    {"result", resultObj}
                };
            }
            packBuildInProgress_ = false;
            packBuildProgress_ = 100;
            packBuildMessage_ = allSuccess ? QStringLiteral("Export complete") : firstError;
            packBuildCancel_.reset();
        };
        QMetaObject::invokeMethod(this, [this] { finishPackBuild(); }, Qt::QueuedConnection);
    });

    packBuildThread_ = buildThread;
    connect(buildThread, &QThread::finished, buildThread, &QThread::deleteLater);
    connect(buildThread, &QThread::finished, this, [this, buildThread]() {
        if (packBuildThread_ == buildThread)
            packBuildThread_ = nullptr;
    });
    buildThread->start();

    return successResponse(QJsonObject{
        {"async", true},
        {"message", "Pack build started. Poll get_pack_build_status for progress."}
    });
}


QJsonObject IPCServer::handleGetPackBuildStatus(const QJsonObject& args)
{
    if (!args.isEmpty()) return errorResponse("INVALID_ARGS", "This query takes no arguments");
    const auto project = packBuildProject_.lock();
    const bool current = project && projectManager_ && projectManager_->currentProject() == project.get();
    return successResponse(QJsonObject{
        {"in_progress", packBuildInProgress_},
        {"progress", packBuildProgress_},
        {"message", packBuildMessage_},
        {"result", current ? packBuildResult_ : QJsonObject{}}
    });
}


QJsonObject IPCServer::handleCancelPackBuild(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!packBuildInProgress_)
    {
        return successResponse(QJsonObject{{"success", true}, {"note", "No build in progress"}});
    }
    
    if (packBuildCancel_)
        packBuildCancel_->store(true, std::memory_order_relaxed);
    packBuildMessage_ = "Canceling pack build...";
    
    return successResponse(QJsonObject{{"success", true}});
}


QJsonObject IPCServer::handleGetPackInfo(const QJsonObject& args)
{
    QString path = args["path"].toString();

    if (path.isEmpty())
        return errorResponse("INVALID_ARGS", "Pack path is required");

    QFileInfo fileInfo(path);
    if (!fileInfo.exists())
        return errorResponse("PACK_NOT_FOUND",
                             QString("Pack file '%1' not found").arg(path));

    QJsonObject result;
    result["path"] = path;
    result["size"] = static_cast<double>(fileInfo.size());
    result["last_modified"] = fileInfo.lastModified().toString(Qt::ISODate);

    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.size() >= 32)
    {
        char magic[4];
        uint32_t version = 0, packID = 0, langID = 0, secCount = 0;
        uint32_t totalSize = 0, crc32 = 0, flags = 0;
        file.read(magic, 4);
        file.read(reinterpret_cast<char*>(&version), 4);
        file.read(reinterpret_cast<char*>(&packID), 4);
        file.read(reinterpret_cast<char*>(&langID), 4);
        file.read(reinterpret_cast<char*>(&secCount), 4);
        file.read(reinterpret_cast<char*>(&totalSize), 4);
        file.read(reinterpret_cast<char*>(&crc32), 4);
        file.read(reinterpret_cast<char*>(&flags), 4);

        result["magic"] = QString::fromLatin1(magic, 4);
        result["version"] = static_cast<int>(version);
        result["packID"] = QString("0x%1").arg(packID, 8, 16, QChar('0'));
        result["sectionCount"] = static_cast<int>(secCount);
        result["crc32"] = QString("0x%1").arg(crc32, 8, 16, QChar('0'));
        result["flags"] = static_cast<int>(flags);
        result["isInitPack"] = (flags & 1) != 0;

        QJsonArray sections;
        QJsonArray codecEntries;
        QJsonArray codecCodebooks;
        qint64 offset = 32;
        for (uint32_t s = 0; s < secCount && offset + 8 <= file.size(); ++s)
        {
            file.seek(offset);
            QByteArray chunkHdr = file.read(8);
            if (chunkHdr.size() < 8) break;

            uint32_t chunkType = 0, chunkSize = 0;
            std::memcpy(&chunkType, chunkHdr.constData(), 4);
            std::memcpy(&chunkSize, chunkHdr.constData() + 4, 4);

            char typeStr[5] = {};
            std::memcpy(typeStr, chunkHdr.constData(), 4);

            QJsonObject sec;
            sec["type"] = QString::fromLatin1(typeStr, 4);
            sec["size"] = static_cast<int>(chunkSize);

            const qint64 payloadOffset = offset + 8;
            if (payloadOffset + static_cast<qint64>(chunkSize) <= file.size()) {
                file.seek(payloadOffset);
                const QByteArray payload = file.read(static_cast<qint64>(chunkSize));
                QByteArray countData = payload.left(4);
                if (countData.size() >= 4) {
                    uint32_t entryCount = 0;
                    std::memcpy(&entryCount, countData.constData(), 4);
                    sec["entryCount"] = static_cast<int>(entryCount);
                }

                if (Bridge::isPackCodecMetadataSection(chunkType)) {
                    QString parseError;
                    codecEntries = inspectPackCodecMetadataSection(payload, &parseError);
                    sec["codecEntryCount"] = codecEntries.size();
                    if (!parseError.isEmpty())
                        sec["codecParseError"] = parseError;
                } else if (Bridge::isPackCodecCodebookSection(chunkType)) {
                    QString parseError;
                    codecCodebooks = inspectPackCodecCodebookSection(payload, &parseError);
                    sec["codebookEntryCount"] = codecCodebooks.size();
                    if (!parseError.isEmpty())
                        sec["codecParseError"] = parseError;
                }
            }

            sections.append(sec);
            offset += 8 + chunkSize;
        }
        result["sections"] = sections;
        result["codec_entries"] = codecEntries;
        result["codec_codebooks"] = codecCodebooks;
        file.close();
    }

    return successResponse(result);
}


QJsonObject IPCServer::handleValidatePack(const QJsonObject& )
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorResponse("NO_PROJECT", "No project is open");

    ProjectPackExporter exporter;
    auto validation = exporter.ValidateBeforeBuild(*project);

    QJsonObject result;
    result["valid"] = validation.valid;
    result["errorCount"] = validation.errorCount;
    result["warningCount"] = validation.warningCount;

    QJsonArray issues;
    for (const auto& issue : validation.issues)
    {
        QJsonObject obj;
        obj["severity"] = (issue.severity == ProjectPackExporter::ValidationIssue::Error) ? "error"
                        : (issue.severity == ProjectPackExporter::ValidationIssue::Warning) ? "warning"
                        : "info";
        obj["message"] = issue.message;
        if (issue.objectID != 0)
            obj["objectID"] = QString("0x%1").arg(issue.objectID, 8, 16, QChar('0'));
        issues.append(obj);
    }
    result["issues"] = issues;

    return successResponse(result);
}


QJsonObject IPCServer::handleGetPackDependencies(const QJsonObject& )
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorResponse("NO_PROJECT", "No project is open");

    ProjectPackExporter exporter;
    QJsonArray tree = exporter.GetDependencyTreeJson(*project);

    QJsonObject result;
    result["dependencies"] = tree;
    result["rootCount"] = tree.size();
    return successResponse(result);
}


QJsonObject IPCServer::handleGetUnusedAssets(const QJsonObject& )
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorResponse("NO_PROJECT", "No project is open");

    ProjectPackExporter exporter;
    QJsonArray unused = exporter.FindUnusedAssetsJson(*project);

    QJsonObject result;
    result["unusedAssets"] = unused;
    result["count"] = unused.size();
    return successResponse(result);
}




QJsonObject IPCServer::handleImportAudio(const QJsonObject& args)
{
    if (!projectManager_ || !projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN",
                             "No project is currently open",
                             "Open or create a project first");
    }

    QString path = args["path"].toString();
    QString packName = args["pack"].toString();
    QString targetParentPath = args["target_parent_path"].toString().trimmed();
    uint32_t targetParentID = static_cast<uint32_t>(args["target_parent_id"].toInteger(0));

    if (path.isEmpty())
    {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "Audio file path is required",
                             "Provide 'path' parameter");
    }

    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    const bool hasExplicitTarget =
        !packName.isEmpty() || targetParentID != 0 || !targetParentPath.isEmpty();

    if (!hasExplicitTarget)
    {
        QStringList files;
        files << path;

        auto* cmd = ImportAssetsCommand::pushImportAtRoot(files);

        if (cmd->successCount() <= 0)
        {
            return errorResponse("IMPORT_FAILED",
                                 "Failed to import audio file",
                                 "Check if the file exists and is a valid audio file");
        }

        QList<uint32_t> ids = cmd->importedIDs();
        QList<QUuid> itemGuids = cmd->importedItemGuids();
        if (!ids.isEmpty())
        {
            uint32_t sourceID = ids.first();
            AssetInfo* info = AssetRegistry::instance().getAsset(sourceID);
            if (info)
            {
                ProjectItem* rootItem = nullptr;
                if (!itemGuids.isEmpty() && !itemGuids.first().isNull())
                    rootItem = ProjectManager::instance().findItemByGuid(itemGuids.first());
                return successResponse(QJsonObject{
                    {"source_id", static_cast<qint64>(sourceID)},
                    {"relative_path", info->relativePath},
                    {"absolute_path", info->absolutePath},
                    {"sample_rate", static_cast<qint64>(info->metadata.sampleRate)},
                    {"channels", info->metadata.channels},
                    {"duration_sec", info->metadata.durationSec},
                    {"target_parent_id", static_cast<qint64>(rootItem ? itemShortID(rootItem) : 0)},
                    {"target_parent_authoring_handle", static_cast<qint64>(rootItem ? rootItem->objectID : 0)},
                    {"target_parent_name", rootItem ? rootItem->name : QString()}
                });
            }
        }

        return successResponse(QJsonObject{
            {"success", true},
            {"count", cmd->successCount()}
        });
    }

    if (packName.isEmpty())
    {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "Pack name is required",
                             "Provide 'pack' parameter");
    }

    ProjectFileBinding* targetBinding = nullptr;
    const auto matches = project->findItemsBySelector(packName);
    if (matches.size() > 1)
        return errorResponse("AMBIGUOUS_SELECTOR", "Multiple objects match this pack name; provide its GUID.");
    for (auto* binding : project->allFileBindings())
    {
        if (matches.size() == 1 && binding && binding->rootItem.get() == matches.front())
        {
            targetBinding = binding;
            break;
        }
    }

    if (!targetBinding)
    {
        return errorResponse("HARKEN_ERR_NOT_FOUND",
                             QString("Pack '%1' not found").arg(packName),
                             "Use list_packs to see available Packs");
    }

    ProjectItem* targetParentByID = nullptr;
    ProjectItem* targetParentByPath = nullptr;
    if (targetParentID != 0)
    {
        targetParentByID = findItemByObjectIDInProject(project, targetParentID);
        if (!targetParentByID)
        {
            return errorResponse("PARENT_NOT_FOUND",
                                 QString("target_parent_id '%1' not found").arg(targetParentID));
        }
    }
    if (!targetParentPath.isEmpty())
    {
        targetParentByPath = findItemByPathInProject(project, targetParentPath);
        if (!targetParentByPath)
        {
            return errorResponse("PARENT_NOT_FOUND",
                                 QString("target_parent_path '%1' not found").arg(targetParentPath));
        }
    }
    if (targetParentByID && targetParentByPath && targetParentByID != targetParentByPath)
    {
        return errorResponse("INVALID_ARGS",
                             "target_parent_id and target_parent_path point to different items");
    }

    ProjectItem* targetParent = targetBinding->rootItem ? targetBinding->rootItem.get() : nullptr;
    if (targetParentByID) {
        targetParent = targetParentByID;
    } else if (targetParentByPath) {
        targetParent = targetParentByPath;
    }

    if (!targetParent)
    {
        return errorResponse("PARENT_NOT_FOUND",
                             "Target binding root is not available");
    }

    ProjectFileBinding* parentBinding = nullptr;
    if (ProjectData* curProject = projectManager_->currentProject())
    {
        parentBinding = findFileBindingForItem(*curProject, targetParent);
    }
    if (parentBinding != targetBinding)
    {
        return errorResponse("INVALID_PARENT",
                             "Import target parent must belong to the target Pack");
    }

    if (!ProjectManager::canAcceptImportedAudioParent(targetParent))
    {
        return errorResponse("INVALID_PARENT",
                             QString("Item '%1' cannot contain imported audio children")
                                 .arg(targetParent->name));
    }

    QStringList files;
    files << path;

    auto* cmd = ImportAssetsCommand::pushImport(files, targetBinding, targetParent);

    if (cmd->successCount() > 0)
    {

        QList<uint32_t> ids = cmd->importedIDs();
        if (!ids.isEmpty())
        {
            uint32_t sourceID = ids.first();
            AssetInfo* info = AssetRegistry::instance().getAsset(sourceID);
            if (info)
            {
                return successResponse(QJsonObject{
                    {"source_id", static_cast<qint64>(sourceID)},
                    {"relative_path", info->relativePath},
                    {"absolute_path", info->absolutePath},
                    {"sample_rate", static_cast<qint64>(info->metadata.sampleRate)},
                    {"channels", info->metadata.channels},
                    {"duration_sec", info->metadata.durationSec},
                    {"target_parent_id", static_cast<qint64>(itemShortID(targetParent))},
                    {"target_parent_authoring_handle", static_cast<qint64>(targetParent->objectID)},
                    {"target_parent_name", targetParent->name}
                });
            }
        }

        return successResponse(QJsonObject{
            {"success", true},
            {"count", cmd->successCount()},
            {"target_parent_id", static_cast<qint64>(itemShortID(targetParent))},
            {"target_parent_authoring_handle", static_cast<qint64>(targetParent->objectID)},
            {"target_parent_name", targetParent->name}
        });
    }
    else
    {
        return errorResponse("IMPORT_FAILED",
                             "Failed to import audio file",
                             "Check if the file exists and is a valid audio file");
    }
}


QJsonObject IPCServer::handleTranscodeAsset(const QJsonObject& args)
{
    uint32_t sourceID = static_cast<uint32_t>(args["source_id"].toInteger());
    const QString platformStr = args["platform_id"].toString();

    if (sourceID == 0)
    {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "source_id is required",
                             "Provide valid source_id from import_audio result");
    }

    const auto platformId = ProjectPlatformSemantics::parseId(platformStr);
    const auto* project = projectManager_ ? projectManager_->currentProject() : nullptr;
    if (args.contains("platform") || !platformId || !project
        || !ProjectPlatformSemantics::find(project->platforms, *platformId))
        return errorResponse("INVALID_PLATFORM", "platform_id must identify a platform in the current project");
    const auto platform = *platformId;

    const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
    if (!asset)
    {
        return errorResponse("HARKEN_ERR_NOT_FOUND",
                             QString("Asset with source_id %1 not found").arg(sourceID),
                             "Use list_assets or import_audio first");
    }

    if (transcodeInProgress_)
    {
        return errorResponse("HARKEN_ERR_BUSY",
                             "A transcode operation is already running",
                             "Wait for the current transcode to complete");
    }

    transcodeInProgress_ = true;
    transcodeResult_ = QJsonObject();

    QString requestError;
    const auto request = AssetRegistry::instance().buildSnapshot().request(sourceID, platform, &requestError);
    if (!request) {
        transcodeInProgress_ = false;
        return errorResponse("INVALID_ASSET", requestError);
    }

    transcodeRequest_ = std::make_shared<MediaConversionRequest>(*request);
    transcodeThread_ = QThread::create([this, sourceID, platformStr, request = *request]() {
        AssetConverter converter;
        ConversionResult result = converter.convert(request);

        QMetaObject::invokeMethod(this, [this, result, sourceID, platformStr, request]() {
            if (!AssetRegistry::instance().accepts(request)) {
                transcodeResult_ = errorResponse("STALE_CONVERSION", "Conversion result discarded because the project, platform or asset changed");
                transcodeInProgress_ = false;
                return;
            }
            if (result.success)
            {
                transcodeResult_ = QJsonObject{
                    {"status", "ok"},
                    {"result", QJsonObject{
                        {"success", true},
                        {"source_id", static_cast<qint64>(sourceID)},
                        {"platform_id", platformStr},
                        {"cache_path", result.cachePath},
                        {"output_size", static_cast<qint64>(result.outputSize)}
                    }}
                };
            }
            else
            {
                transcodeResult_ = QJsonObject{
                    {"status", "error"},
                    {"code", "HARKEN_ERR_CONVERSION_FAILED"},
                    {"message", result.errorMessage}
                };
            }
            transcodeInProgress_ = false;
        }, Qt::QueuedConnection);
    });

    connect(transcodeThread_, &QThread::finished, transcodeThread_, &QThread::deleteLater);
    connect(transcodeThread_, &QThread::finished, this, [this, thread = transcodeThread_]() {
        if (transcodeThread_ == thread) transcodeThread_ = nullptr;
    });
    transcodeThread_->start();

    return successResponse(QJsonObject{
        {"async", true},
        {"source_id", static_cast<qint64>(sourceID)},
        {"platform_id", platformStr},
        {"message", "Transcode started. Poll get_transcode_status for result."}
    });
}



QJsonObject IPCServer::handleGetConversionSettings(const QJsonObject& args)
{
    uint32_t sourceID = static_cast<uint32_t>(args["source_id"].toInteger());
    if (sourceID == 0)
        return errorResponse("HARKEN_ERR_MISSING_PARAM", "source_id is required");

    const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
    if (!asset)
        return errorResponse("HARKEN_ERR_NOT_FOUND",
                             QString("Asset %1 not found").arg(sourceID));

    const ConversionSettings* override_ =
        PlatformProfileManager::instance().getFileOverride(sourceID);

    QJsonObject settingsJson = override_ ? override_->toJson() : asset->settings.toJson();
    settingsJson["has_override"] = (override_ != nullptr);

    return successResponse(settingsJson);
}


QJsonObject IPCServer::handleSetConversionSettings(const QJsonObject& args)
{
    uint32_t sourceID = static_cast<uint32_t>(args["source_id"].toInteger());
    if (sourceID == 0)
        return errorResponse("HARKEN_ERR_MISSING_PARAM", "source_id is required");

    AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
    if (!asset)
        return errorResponse("HARKEN_ERR_NOT_FOUND",
                             QString("Asset %1 not found").arg(sourceID));

    ConversionSettings settings = asset->settings;
    const uint32_t oldProcessingHash = settings.hash();

    if (args.contains("codec")) {
        settings.format = ConversionSettings::codecFromString(args["codec"].toString());
    }
    if (args.contains("quality"))
        settings.vorbisQuality = static_cast<float>(args["quality"].toDouble());
    if (args.contains("resample"))
        settings.performResample = args["resample"].toBool();
    if (args.contains("target_sample_rate"))
        settings.targetSampleRate = static_cast<uint32_t>(args["target_sample_rate"].toInteger());
    if (args.contains("force_mono") || args.contains("trim_silence") || args.contains("trim_threshold_db")
        || args.contains("normalize") || args.contains("normalize_target_db"))
        return errorResponse("INVALID_ARGS", "Mono conversion, silence trimming and normalization settings are no longer supported");

    asset->settings = settings;

    bool asOverride = args["as_override"].toBool(false);
    if (asOverride)
        PlatformProfileManager::instance().setFileOverride(sourceID, settings);

    if (settings.hash() != oldProcessingHash)
        AssetRegistry::instance().invalidateAllCaches(sourceID);

    return successResponse(QJsonObject{
        {"source_id", static_cast<qint64>(sourceID)},
        {"settings", settings.toJson()}
    });
}


QJsonObject IPCServer::handleGetPlatformProfile(const QJsonObject& args)
{
    const auto id = ProjectPlatformSemantics::parseId(args["platform_id"].toString());
    const auto* project = projectManager_ ? projectManager_->currentProject() : nullptr;
    const auto* platform = project && id ? ProjectPlatformSemantics::find(project->platforms, *id) : nullptr;
    if (args.contains("platform") || !platform)
        return errorResponse("INVALID_PLATFORM", "platform_id must identify a platform in the current project");
    return successResponse(QJsonObject{
        {"platform", platform->toJson()},
        {"effective_profile", platform->profile.toJson()}});
}


QJsonObject IPCServer::handleSetPlatformProfile(const QJsonObject&)
{
    return errorResponse("PLATFORM_INSTANCES_REQUIRED",
        "Use set_project_platform_profile with platform_id and profile; global platform profiles are no longer editable");
}


QJsonObject IPCServer::handleGetAssetInfo(const QJsonObject& args)
{
    uint32_t sourceID = static_cast<uint32_t>(args["source_id"].toInteger());

    if (sourceID == 0)
    {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "source_id is required");
    }

    auto& registry = AssetRegistry::instance();
    const AssetInfo* asset = registry.getAsset(sourceID);
    if (!asset)
    {
        return errorResponse("HARKEN_ERR_NOT_FOUND",
                             QString("Asset with source_id %1 not found (registry has %2 assets)")
                                 .arg(sourceID).arg(registry.count()));
    }

    QJsonObject cacheStatus;
    for (auto it = asset->platformCaches.begin(); it != asset->platformCaches.end(); ++it)
    {
        cacheStatus[ProjectPlatformSemantics::idString(it.key())] = QJsonObject{
            {"valid", it.value().valid},
            {"cache_path", it.value().cachePath},
            {"last_access", static_cast<qint64>(it.value().lastAccessTime)}
        };
    }

    return successResponse(QJsonObject{
        {"source_id", static_cast<qint64>(asset->sourceID)},
        {"relative_path", asset->relativePath},
        {"absolute_path", asset->absolutePath},
        {"is_orphaned", asset->isOrphaned},
        {"last_modified", static_cast<qint64>(asset->lastModified)},
        {"metadata", asset->metadata.toJson()},
        {"settings", asset->settings.toJson()},
        {"cache_status", cacheStatus}
    });
}


QJsonObject IPCServer::handleCleanupCache(const QJsonObject& args)
{
    int maxAgeDays = args["max_age_days"].toInt(90);
    qint64 maxSizeBytes = args["max_size_bytes"].toInteger(0);

    AssetGarbageCollector gc;
    gc.setProjectRoot(AssetRegistry::instance().projectRoot());
    CleanupResult result = gc.cleanupLRU(maxAgeDays, static_cast<uint64_t>(maxSizeBytes));

    return successResponse(QJsonObject{
        {"deleted_count", result.deletedCount},
        {"freed_bytes", static_cast<qint64>(result.freedBytes)},
        {"deleted_files", QJsonArray::fromStringList(result.deletedFiles)}
    });
}


QJsonObject IPCServer::handleListAssets(const QJsonObject& args)
{
    Q_UNUSED(args)

    QJsonArray assetArray;
    for (const AssetInfo* asset : AssetRegistry::instance().getAllAssets())
    {
        assetArray.append(QJsonObject{
            {"source_id", static_cast<qint64>(asset->sourceID)},
            {"relative_path", asset->relativePath},
            {"format", asset->metadata.originalFormat},
            {"duration_sec", asset->metadata.durationSec},
            {"sample_rate", static_cast<qint64>(asset->metadata.sampleRate)},
            {"channels", asset->metadata.channels},
            {"is_orphaned", asset->isOrphaned}
        });
    }

    return successResponse(QJsonObject{
        {"count", assetArray.size()},
        {"assets", assetArray}
    });
}



QJsonObject IPCServer::handleGetMediaStatus(const QJsonObject& args)
{
    QJsonArray sourceIDsArr = sourceIDArrayFromArgs(args);
    if (sourceIDsArr.isEmpty()) {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "source_ids array is required",
                             "Provide an array of source_id integers");
    }

    auto& pool = MediaPool::instance();
    const auto id = ProjectPlatformSemantics::parseId(args["platform_id"].toString());
    const auto* project = projectManager_ ? projectManager_->currentProject() : nullptr;
    if (args.contains("platform") || !id || !project
        || !ProjectPlatformSemantics::find(project->platforms, *id))
        return errorResponse("INVALID_PLATFORM", "platform_id must identify a platform in the current project");
    const auto platform = *id;

    QJsonObject statusMap;
    for (const auto& val : sourceIDsArr) {
        uint32_t id = static_cast<uint32_t>(val.toInteger());
        MediaStatus status = pool.computeStatus(id, platform);
        statusMap[QString::number(id)] = mediaStatusToString(status);
    }

    PoolStats stats = pool.getStats(platform);

    return successResponse(QJsonObject{
        {"platform_id", ProjectPlatformSemantics::idString(platform)},
        {"statuses", statusMap},
        {"stats", stats.toJson()}
    });
}


QJsonObject IPCServer::handleRescanMediaSources(const QJsonObject& args)
{
    auto& pool = MediaPool::instance();
    const auto id = ProjectPlatformSemantics::parseId(args["platform_id"].toString());
    const auto* project = projectManager_ ? projectManager_->currentProject() : nullptr;
    if (args.contains("platform") || !id || !project
        || !ProjectPlatformSemantics::find(project->platforms, *id))
        return errorResponse("INVALID_PLATFORM", "platform_id must identify a platform in the current project");
    const auto platform = *id;
    const int queued = pool.rescan(platform);

    return successResponse(QJsonObject{
        {"queued", queued},
        {"platform_id", ProjectPlatformSemantics::idString(platform)},
        {"message", QStringLiteral("Media source status rescan queued.")}
    });
}


QJsonObject IPCServer::handleBatchReplaceMedia(const QJsonObject& args)
{
    QMap<uint32_t, QString> replacements = replacementMapFromArgs(args);
    if (replacements.isEmpty()) {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "replacements object is required",
                             "Provide replacements {\"123\": \"/path.wav\"} or items [{source_id, path}]");
    }

    auto& pool = MediaPool::instance();
    auto results = pool.batchReplace(replacements);

    QJsonArray resultsArr;
    int succeeded = 0, failed = 0;
    for (const auto& r : results) {
        QJsonObject entry;
        entry["source_id"] = static_cast<qint64>(r.sourceID);
        entry["sourceID"] = static_cast<qint64>(r.sourceID);
        entry["success"] = r.success;
        if (!r.success)
            entry["error"] = r.errorMessage;
        resultsArr.append(entry);

        if (r.success) ++succeeded;
        else ++failed;
    }

    return successResponse(QJsonObject{
        {"results", resultsArr},
        {"succeeded", succeeded},
        {"failed", failed}
    });
}


QJsonObject IPCServer::handleBatchConvertMedia(const QJsonObject& args)
{
    QJsonArray sourceIDsArr = sourceIDArrayFromArgs(args);
    if (sourceIDsArr.isEmpty()) {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "source_ids array is required",
                             "Provide an array of source_id integers");
    }

    const auto id = ProjectPlatformSemantics::parseId(args["platform_id"].toString());
    const auto* project = projectManager_ ? projectManager_->currentProject() : nullptr;
    if (args.contains("platform") || !id || !project
        || !ProjectPlatformSemantics::find(project->platforms, *id))
        return errorResponse("INVALID_PLATFORM", "platform_id must identify a platform in the current project");
    const auto platform = *id;

    QVector<uint32_t> sourceIDs;
    sourceIDs.reserve(sourceIDsArr.size());
    for (const auto& val : sourceIDsArr) {
        uint32_t id = static_cast<uint32_t>(val.toInteger());
        if (id != 0)
            sourceIDs.append(id);
    }

    auto& pool = MediaPool::instance();
    pool.batchConvert(sourceIDs, platform);

    return successResponse(QJsonObject{
        {"queued", sourceIDs.size()},
        {"platform_id", ProjectPlatformSemantics::idString(platform)},
        {"message", QStringLiteral("Batch conversion started. Monitor via get_media_status.")}
    });
}


}
