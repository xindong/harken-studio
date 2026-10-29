#include "ProjectPackBuildService.h"

#include "ProjectBuildSnapshot.h"

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
    for (const auto& platform : request.platforms) {
        if (!ProjectPlatformSemantics::find(project.platforms, QUuid(platform))) {
            result.preflightError = QStringLiteral("Unknown project platform ID: %1").arg(platform);
            return result;
        }
    }
    const auto outputLock = request.outputLock ? request.outputLock
        : ProjectPackExporter::LockPackOutputRoot(request.outputRoot, &result.preflightError);
    result.outputLocked = static_cast<bool>(outputLock);
    if (!result.outputLocked) return result;
    const auto canceled = [&] {
        return request.options.cancelCheck && request.options.cancelCheck();
    };
    if (request.options.observationCallback)
        request.options.observationCallback({{}, {}, {}, PackBuildObservationStage::Preflight});
    result.preflightSucceeded = ProjectPackExporter::ValidatePackBuildWriteAccess(
        project, request.outputRoot, request.platforms, request.options,
        &result.preflightError, &result.preflightSummary);
    result.allPlatformsSucceeded = result.preflightSucceeded;
    if (!result.preflightSucceeded) {
        result.canceled = canceled();
        return result;
    }
    if (request.preflightFinished) request.preflightFinished();
    ProjectPackExporter exporter;
    auto options = request.options;
    options.selfCleaning = false;
    QMap<QString, QVector<PackArtifactMetadata>> artifactsByPlatform;
    for (const auto& platform : request.platforms) {
        if (canceled()) {
            result.canceled = true;
            break;
        }
        options.platform = platform;
        PlatformResult built;
        built.platform = platform;
        built.timer.start();
        built.definition = *ProjectPlatformSemantics::find(project.platforms, QUuid(platform));
        built.outputDir = ProjectPackExporter::platformOutputDir(request.outputRoot, built.definition);
        built.result = exporter.ExportAllPacks(project, built.outputDir, options, &built.records);
        if (built.result.success) {
            result.succeededPlatforms.append(platform);
            if (request.artifactOrder == ArtifactOrder::PlatformId)
                artifactsByPlatform.insert(platform, built.result.artifacts);
            else
                result.artifacts.append(built.result.artifacts);
        }
        result.allPlatformsSucceeded = result.allPlatformsSucceeded && built.result.success;
        if (request.platformFinished) request.platformFinished(built);
        const bool platformCanceled = built.result.canceled;
        result.platforms.push_back(std::move(built));
        if (request.stopOnCanceledPlatform) {
            result.canceled = platformCanceled || canceled();
            if (result.canceled) break;
        }
    }
    result.canceled = result.canceled || canceled();
    if (request.artifactOrder == ArtifactOrder::PlatformId)
        for (const auto& artifacts : artifactsByPlatform) result.artifacts.append(artifacts);
    if (result.canceled) return result;
    const auto& finalizationCanceled = request.finalizationCancelCheck
        ? request.finalizationCancelCheck : request.options.cancelCheck;
    if (request.options.observationCallback)
        request.options.observationCallback({{}, {}, {}, PackBuildObservationStage::Finalizing});
    if (result.allPlatformsSucceeded) {
        result.finalization = Finalization::Complete;
        result.finalizationResult = ProjectPackExporter::FinalizePackExport(
            request.outputRoot, project, request.platforms, request.options.selfCleaning,
            result.artifacts, finalizationCanceled);
        result.canceled = result.finalizationResult.canceled && canceled();
    } else {
        result.finalization = Finalization::Partial;
        result.finalizationResult.success = ProjectPackExporter::FinalizePackOutputs(
            request.outputRoot, project.platforms, request.platforms, result.succeededPlatforms,
            &result.finalizationResult.error, result.artifacts, finalizationCanceled);
        result.canceled = !result.finalizationResult.success && canceled();
    }
    return result;
}

}
