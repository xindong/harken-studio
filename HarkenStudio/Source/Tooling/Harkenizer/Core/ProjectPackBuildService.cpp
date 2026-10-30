#include "Tooling/Harkenizer/Core/ProjectPackBuildService.h"

#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"
#include "Tooling/Harkenizer/Core/PackBuildReceipt.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Tooling/Harkenizer/BuildIdentity.h"
#include "Tooling/Harkenizer/ResourceCompilerClient.h"
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
    for (const auto& platform : request.platforms) {
        if (!ProjectPlatformSemantics::find(project.platforms, QUuid(platform))) {
            result.preflightError = QStringLiteral("Unknown project platform ID: %1").arg(platform);
            return result;
        }
    }
    if (PathUtils::isProtectedPackOutputPath(request.outputRoot, project.projectPath)) {
        result.preflightError = PathUtils::packBuildOutputLocationError();
        return result;
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
    auto options = request.options;
    std::map<QString, PackBuildReceipt> receipts;
    std::map<QString, PlatformResult> restored;
    QSet<QString> reusablePlatforms;
    for (const auto& platform : request.platforms) {
        if (canceled()) {
            result.canceled = true;
            return result;
        }
        const auto& definition = *ProjectPlatformSemantics::find(project.platforms, QUuid(platform));
        const QDir directory(ProjectPackExporter::platformOutputDir(request.outputRoot, definition));
        if (!ProjectPackExporter::PreparePackOutputDirectory(request.outputRoot, definition,
                &result.preflightError)) return result;
        auto lock = std::make_shared<QLockFile>(directory.filePath(QStringLiteral(".harken-pack-platform.lock")));
        lock->setStaleLockTime(0);
        if (!lock->tryLock()) {
            result.preflightError = QStringLiteral("Platform output is being built by another request: %1").arg(directory.path());
            return result;
        }
        options.platformLocks.insert(platform, std::move(lock));
        if (!RecoverPackOutput(directory.path(), &result.preflightError)) return result;
        options.platform = platform;
        auto receipt = PackBuildReceipt::prepare(options.snapshot, options, directory.path(),
            QByteArray(Harkenizer::projectBuildIdentity()));
        if (!receipt) continue;
        PlatformResult cached;
        if (receipt->restore(&cached.result, &cached.records)) {
            reusablePlatforms.insert(platform);
            restored.emplace(platform, std::move(cached));
        }
        receipts.emplace(platform, std::move(*receipt));
    }
    if (reusablePlatforms.size() != request.platforms.size())
        options.buildContext = ProjectPackExporter::createBuildContext(project);
    result.preflightSucceeded = ProjectPackExporter::ValidatePackBuildWriteAccess(
        project, request.outputRoot, request.platforms, options,
        &result.preflightError, &result.preflightSummary, reusablePlatforms);
    result.allPlatformsSucceeded = result.preflightSucceeded;
    if (!result.preflightSucceeded) {
        result.canceled = canceled();
        return result;
    }
    if (request.preflightFinished) request.preflightFinished();
    ProjectPackExporter exporter;
    if (!options.resourceCompiler)
        options.resourceCompiler = std::make_shared<Harkenizer::ResourceCompilerClient>();
    options.selfCleaning = false;
    QMap<QString, QVector<PackArtifactMetadata>> artifactsByPlatform;
    QMap<QString, PackPublicationProof> publications;
    QMap<QString, QMap<uint32_t, QString>> sourceDependencies;
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
        const auto cached = restored.find(platform);
        if (cached != restored.end()) {
            built.result = std::move(cached->second.result);
            built.records = std::move(cached->second.records);
            if (options.progressCallback)
                for (const auto& record : built.records)
                    options.progressCallback({platform, record.language, record.unitKey,
                        100, PackGenerationStage::Skipped});
        } else {
            if (options.snapshot->media.sourceInputs)
                options.snapshot->media.sourceInputs->beginDependencyScope();
            built.result = exporter.ExportAllPacks(project, built.outputDir, options, &built.records);
            if (options.snapshot->media.sourceInputs)
                sourceDependencies.insert(platform, options.snapshot->media.sourceInputs->fingerprints());
        }
        if (built.result.success) {
            if (built.result.publication)
                publications.insert(platform, *built.result.publication);
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
    if ((options.snapshot->media.sourceInputs
            && !options.snapshot->media.sourceInputs->verifyUnchanged(&result.preflightError, canceled))
        || (options.snapshot->derivativeCache
            && !options.snapshot->derivativeCache->verifyUnchanged(&result.preflightError, canceled))) {
        result.allPlatformsSucceeded = false;
        result.canceled = canceled();
        result.finalization = Finalization::Complete;
        result.finalizationResult.error = result.preflightError;
        result.finalizationResult.canceled = result.canceled;
        return result;
    }
    const auto& finalizationCanceled = request.finalizationCancelCheck
        ? request.finalizationCancelCheck : request.options.cancelCheck;
    if (request.options.observationCallback)
        request.options.observationCallback({{}, {}, {}, PackBuildObservationStage::Finalizing});
    if (result.allPlatformsSucceeded) {
        result.finalization = Finalization::Complete;
        result.finalizationResult = ProjectPackExporter::FinalizePackExport(
            request.outputRoot, project, request.platforms, request.options.selfCleaning,
            result.artifacts, finalizationCanceled, publications);
        result.canceled = result.finalizationResult.canceled && canceled();
    } else {
        result.finalization = Finalization::Partial;
        result.finalizationResult.success = ProjectPackExporter::FinalizePackOutputs(
            request.outputRoot, project.platforms, request.platforms, result.succeededPlatforms,
            &result.finalizationResult.error, result.artifacts, finalizationCanceled, publications);
        result.canceled = !result.finalizationResult.success && canceled();
    }
    if (result.finalizationResult.success && !result.canceled) {
        for (const auto& built : result.platforms) {
            if (reusablePlatforms.contains(built.platform)) continue;
            const auto receipt = receipts.find(built.platform);
            if (receipt != receipts.end())
                receipt->second.save(built.result, built.records,
                    built.result.reusableBuild && built.result.mediaFallbacks == 0,
                    sourceDependencies.value(built.platform));
        }
    }
    return result;
}

}
