#include "Tooling/Harkenizer/Core/ProjectPackBuildService.h"

#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "Tooling/Harkenizer/Core/PackBuildState.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Tooling/Harkenizer/BuildIdentity.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/PathUtils.h"

#include <QDir>
#include <QLockFile>
#include <QMap>

#include <utility>

namespace VNX::ProjectPackBuildService {

Result run(const Request& request)
{
    Result result;
    if (!request.options.snapshot) {
        result.preflightError = QStringLiteral("A project build snapshot is required");
        return result;
    }
    const auto& project = request.options.snapshot->project;
    if (request.platforms.isEmpty()) {
        result.preflightError = QStringLiteral("Select at least one platform to export");
        return result;
    }
    if (PathUtils::isProtectedPackOutputPath(request.outputRoot, project.projectPath)) {
        result.preflightError = PathUtils::packBuildOutputLocationError();
        return result;
    }
    const auto outputLock = request.outputLock ? request.outputLock
        : ProjectPackExporter::LockPackOutputRoot(request.outputRoot, &result.preflightError);
    result.outputLocked = static_cast<bool>(outputLock);
    if (!result.outputLocked) return result;
    const auto cacheDirectory = QDir(PathUtils::getDotCacheDir(request.options.snapshot->media.projectRoot))
        .filePath(QStringLiteral("pack"));
    if (!QDir().mkpath(cacheDirectory)) {
        result.preflightError = QStringLiteral("Cannot create the Pack cache directory: %1").arg(cacheDirectory);
        return result;
    }
    QLockFile cacheLock(QDir(cacheDirectory).filePath(QStringLiteral(".build.lock")));
    cacheLock.setStaleLockTime(0);
    if (!cacheLock.tryLock()) {
        result.preflightError = QStringLiteral("This project's Packs are being built by another request");
        return result;
    }
    const auto canceled = [&] {
        return request.options.cancelCheck && request.options.cancelCheck();
    };
    if (request.options.snapshot->media.sourceInputs)
        request.options.snapshot->media.sourceInputs->beginRequest();
    if (request.options.snapshot->derivativeCache)
        request.options.snapshot->derivativeCache->clear();
    auto options = request.options;
    options.buildContext = ProjectPackExporter::createBuildContext(project);
    options.packBuildState = std::make_shared<PackBuildState>(
        options.snapshot, QByteArray(Harkenizer::projectBuildIdentity()));
    options.selfCleaning = false;
    result.preflightSucceeded = true;
    result.allPlatformsSucceeded = true;
    if (request.preflightFinished) request.preflightFinished();
    ProjectPackExporter exporter;
    QMap<QString, QVector<PackArtifactMetadata>> artifactsByPlatform;
    QMap<QString, QJsonArray> outputs;
    QStringList knownPlatforms;
    bool cacheUsed = false;
    for (const auto& platform : request.platforms) {
        if (canceled()) {
            result.canceled = true;
            break;
        }
        PlatformResult built;
        built.platform = platform;
        built.timer.start();
        const auto* definition = ProjectPlatformSemantics::find(project.platforms, QUuid(platform));
        if (!definition) {
            built.result.error = QStringLiteral("Unknown project platform ID: %1").arg(platform);
        } else {
            knownPlatforms.append(platform);
            built.definition = *definition;
            built.outputDir = ProjectPackExporter::platformOutputDir(request.outputRoot, *definition);
            built.result.outputPath = built.outputDir;
            if (ProjectPackExporter::PreparePackOutputDirectory(request.outputRoot, *definition,
                    &built.result.error)) {
                auto lock = std::make_shared<QLockFile>(
                    QDir(built.outputDir).filePath(QStringLiteral(".harken-pack-platform.lock")));
                lock->setStaleLockTime(0);
                if (!lock->tryLock()) {
                    built.result.error = QStringLiteral("Platform output is being built by another request: %1")
                        .arg(built.outputDir);
                } else {
                    options.platform = platform;
                    options.platformLocks.insert(platform, std::move(lock));
                    options.buildPlan = ProjectPackExporter::createBuildPlan(project, options);
                    options.packBuildState->select(options, built.outputDir);
                    if (!options.forceReexport
                        && options.packBuildState->restore(&built.result, &built.records)) {
                        if (options.progressCallback)
                            for (const auto& record : built.records)
                                options.progressCallback({platform, record.language, record.unitKey,
                                    100, PackGenerationStage::Skipped});
                    } else {
                        cacheUsed = true;
                        built.result = exporter.ExportAllPacks(project, built.outputDir, options, &built.records);
                        options.packBuildState->save(built.result, built.records);
                    }
                    if (built.result.success) options.packBuildState->applyCounts(built.result);
                }
            }
        }
        if (built.result.success) result.succeededPlatforms.append(platform);
        if (request.artifactOrder == ArtifactOrder::PlatformId)
            artifactsByPlatform.insert(platform, built.result.artifacts);
        else
            result.artifacts.append(built.result.artifacts);
        outputs.insert(platform, built.result.outputs);
        result.allPlatformsSucceeded = result.allPlatformsSucceeded && built.result.success;
        result.canceled = built.result.canceled || canceled();
        if (request.platformFinished) request.platformFinished(built);
        result.platforms.push_back(std::move(built));
        if (result.canceled) break;
    }
    result.allPlatformsSucceeded = result.allPlatformsSucceeded && !result.canceled;
    if (request.artifactOrder == ArtifactOrder::PlatformId)
        for (const auto& artifacts : artifactsByPlatform) result.artifacts.append(artifacts);
    if (!result.canceled) {
        if (request.options.observationCallback)
            request.options.observationCallback({{}, {}, {}, PackBuildObservationStage::Finalizing});
        const bool selfCleaning = request.options.selfCleaning
            && !request.options.updateSelectedOnly && result.allPlatformsSucceeded;
        result.finalization = Finalization::Complete;
        result.finalizationResult = ProjectPackExporter::FinalizePackExport(
            request.outputRoot, project, knownPlatforms, selfCleaning,
            outputs, request.options.cancelCheck);
        if (selfCleaning && result.finalizationResult.success) {
            QMap<QString, QJsonArray> retained;
            for (const auto& built : result.platforms)
                retained.insert(QDir(built.outputDir).absolutePath(), built.result.outputs);
            options.packBuildState->retainOutputs(request.outputRoot, retained);
        }
        result.canceled = result.finalizationResult.canceled || canceled();
    }
    if (cacheUsed) options.packBuildState->prune();
    if (!options.packBuildState->flush())
        result.finalizationResult.warnings.append(QStringLiteral("Could not save Pack build state"));
    if (options.snapshot->media.sourceInputs && !options.snapshot->media.sourceInputs->save())
        result.finalizationResult.warnings.append(QStringLiteral("Could not save source build state"));
    return result;
}

}
