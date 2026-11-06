#include "SavedProjectBuild.h"
#include "StudioSession.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectLocator.h"
#include "Core/SchemaArchive.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "Tooling/Harkenizer/Core/ProjectLoader.h"
#include "Tooling/Harkenizer/Core/ProjectPackBuildService.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonArray>
#include <QTextStream>

namespace HarkenStudioCli {
namespace {

QString progressMessage(const VNX::PackBuildObservation& observation, const VNX::ProjectData& project)
{
    const auto* platform = VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(observation.platform));
    const QString prefix = platform ? QStringLiteral("[%1] ").arg(platform->name) : QString();
    const QString unit = observation.language.isEmpty() ? observation.packName
        : observation.language + QLatin1Char('/') + observation.packName;
    switch (observation.stage) {
    case VNX::PackBuildObservationStage::PreparingMedia:
        return prefix + QStringLiteral("Preparing media");
    case VNX::PackBuildObservationStage::Preparing:
        return prefix + QStringLiteral("Preparing media: ") + unit;
    case VNX::PackBuildObservationStage::CompilingProject:
        return prefix + QStringLiteral("Compiling project");
    case VNX::PackBuildObservationStage::WritingPacks:
        return prefix + QStringLiteral("Writing packs");
    case VNX::PackBuildObservationStage::Writing:
        return prefix + QStringLiteral("Writing pack: ") + unit;
    case VNX::PackBuildObservationStage::Finalizing:
        return QStringLiteral("Finalizing outputs");
    default:
        return {};
    }
}

QJsonObject listTargets(const VNX::ProjectData& project)
{
    QJsonArray platforms;
    for (const auto& platform : project.platforms) {
        platforms.append(QJsonObject{
            {QStringLiteral("id"), VNX::ProjectPlatformSemantics::idString(platform.id)},
            {QStringLiteral("name"), platform.name},
            {QStringLiteral("basePlatform"), VNX::ProjectPlatformSemantics::basePlatformName(platform.basePlatform)}});
    }
    QJsonArray actions;
    for (const auto& action : VNX::ProjectPackExporter::CollectActionPackUnits(project)) {
        actions.append(QJsonObject{
            {QStringLiteral("stableId"), VNX::objectIdString(action.stableId)},
            {QStringLiteral("name"), action.name}});
    }
    return {{QStringLiteral("projectId"), project.projectId},
        {QStringLiteral("platforms"), platforms},
        {QStringLiteral("languages"), QJsonArray::fromStringList(VNX::ProjectLanguageSemantics::buildLanguages(project))},
        {QStringLiteral("actions"), actions}};
}

}

QJsonObject runSavedProjectBuild(const QCommandLineParser& parser,
                                const std::function<bool()>& canceled)
{
    const bool listing = parser.positionalArguments().front() == QStringLiteral("list-targets");
    const QStringList allowed = listing
        ? QStringList{QStringLiteral("project"), QStringLiteral("json"), QStringLiteral("verbose")}
        : QStringList{QStringLiteral("project"), QStringLiteral("json"), QStringLiteral("verbose"),
            QStringLiteral("all"), QStringLiteral("platform-id"), QStringLiteral("self-clean"),
            QStringLiteral("output"), QStringLiteral("force")};
    for (const auto& option : parser.optionNames()) {
        if (!allowed.contains(option))
            return failure(QStringLiteral("USAGE"), QStringLiteral("--%1 is not supported by %2.")
                .arg(option, parser.positionalArguments().front()));
    }
    if (parser.positionalArguments().size() != 1 || parser.values(QStringLiteral("project")).size() != 1
        || parser.value(QStringLiteral("project")).trimmed().isEmpty())
        return failure(QStringLiteral("USAGE"), QStringLiteral("Specify exactly one saved project with --project <project.hsp>."));
    if (!listing && (!parser.isSet(QStringLiteral("all"))
        || parser.values(QStringLiteral("platform-id")).isEmpty()
        || parser.values(QStringLiteral("self-clean")).size() != 1
        || (parser.value(QStringLiteral("self-clean")) != QStringLiteral("on")
            && parser.value(QStringLiteral("self-clean")) != QStringLiteral("off"))
        || parser.values(QStringLiteral("output")).size() > 1
        || (parser.isSet(QStringLiteral("output")) && parser.value(QStringLiteral("output")).trimmed().isEmpty())))
        return failure(QStringLiteral("USAGE"), QStringLiteral(
            "Use build --project <project.hsp> --all --platform-id <UUID> --self-clean <on|off> [--output <directory>] [--force] [--json]."));

    QStringList platforms;
    for (const QString& value : parser.values(QStringLiteral("platform-id"))) {
        const auto id = VNX::ProjectPlatformSemantics::parseId(value);
        if (!id) return failure(QStringLiteral("USAGE"), QStringLiteral("Invalid project platform UUID: %1").arg(value));
        const QString canonical = VNX::ProjectPlatformSemantics::idString(*id);
        if (platforms.contains(canonical))
            return failure(QStringLiteral("USAGE"), QStringLiteral("A platform was requested more than once: %1").arg(value));
        platforms.append(canonical);
    }
    const QFileInfo projectFile(parser.value(QStringLiteral("project")));
    if (!projectFile.isFile() || projectFile.suffix().compare(QStringLiteral("hsp"), Qt::CaseInsensitive) != 0)
        return failure(QStringLiteral("INVALID_PROJECT"), QStringLiteral("Project must be an existing .hsp file: %1")
            .arg(parser.value(QStringLiteral("project"))));
    const auto location = VNX::ProjectLocator::locate(projectFile.absoluteFilePath());
    if (!location.ok())
        return failure(VNX::projectIssueCode(location.issue), QStringLiteral("Cannot locate a unique Harken project: %1")
            .arg(projectFile.absoluteFilePath()));
    const auto instance = VNX::ProjectInstance::acquire(location.projectFilePath, VNX::ProjectInstance::ActivationPolicy::Deny);
    if (instance.status != VNX::ProjectInstance::Status::Acquired)
        return failure(QStringLiteral("PROJECT_IN_USE"), instance.error);
    QString schemaError;
    if (!VNX::SchemaArchive::instance().verifyBuildSnapshot(&schemaError))
        return failure(QStringLiteral("SCHEMA_UNAVAILABLE"), schemaError);
    auto prepared = VNX::ProjectLoader::prepareProject(location.projectFilePath, false, {}, {}, canceled,
        nullptr, VNX::ProjectSerializer::LoadMode::ReadOnly);
    if (canceled()) return failure(QStringLiteral("CANCELED"), QStringLiteral("Canceled."));
    if (!prepared.project) return failure(QStringLiteral("OPEN_PROJECT_FAILED"), prepared.error);
    for (const auto& diagnostic : prepared.diagnostics) QTextStream(stderr) << diagnostic << Qt::endl;
    if (listing)
        return {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("result"), listTargets(*prepared.project)}};

    auto& project = *prepared.project;
    for (const auto& platform : platforms) {
        if (!VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(platform)))
            return failure(QStringLiteral("INVALID_PLATFORM"), QStringLiteral("Unknown project platform UUID: %1").arg(platform));
    }
    const QString projectRoot = QFileInfo(project.projectPath).absolutePath();
    auto& assets = VNX::AssetRegistry::instance();
    assets.replaceSnapshot(projectRoot, prepared.assets);
    assets.setPlatforms(project.platforms);
    QString snapshotError;
    const auto snapshot = VNX::ProjectBuildSnapshot::capture(project, &snapshotError);
    if (!snapshot) return failure(QStringLiteral("BUILD_SNAPSHOT_FAILED"), snapshotError);
    const QString outputRoot = parser.isSet(QStringLiteral("output"))
        ? VNX::PathUtils::toAbsolutePath(parser.value(QStringLiteral("output")), projectRoot)
        : VNX::PathUtils::packBuildOutputRoot(project.projectPath, project.packBuildOutputDir);
    if (VNX::PathUtils::isProtectedPackOutputPath(outputRoot, project.projectPath))
        return failure(QStringLiteral("INVALID_OUTPUT"), VNX::PathUtils::packBuildOutputLocationError());
    const QFileInfo outputInfo(outputRoot);
    if (outputInfo.isFile() || outputInfo.fileName().endsWith(QStringLiteral(".bytes"), Qt::CaseInsensitive)
        || outputInfo.fileName().endsWith(QStringLiteral(".pack"), Qt::CaseInsensitive))
        return failure(QStringLiteral("INVALID_OUTPUT"), QStringLiteral("Output must be a directory, not a pack file."));

    VNX::ProjectPackBuildService::Request request;
    request.outputRoot = outputRoot;
    request.platforms = platforms;
    request.options.snapshot = snapshot;
    request.options.includeUnchecked = true;
    request.options.selection.selectedPlatformIds = platforms;
    request.options.selection.currentPlatformId = platforms.front();
    request.options.forceReexport = parser.isSet(QStringLiteral("force"));
    request.options.selfCleaning = parser.value(QStringLiteral("self-clean")) == QStringLiteral("on");
    request.options.cancelCheck = canceled;
    QString lockError;
    request.outputLock = VNX::ProjectPackExporter::LockPackOutputRoot(outputRoot, &lockError);
    if (!request.outputLock) return failure(QStringLiteral("OUTPUT_LOCK_FAILED"), lockError);
    QString lastProgress;
    request.options.observationCallback = [&lastProgress, &project](const VNX::PackBuildObservation& observation) {
        const auto message = progressMessage(observation, project);
        if (message.isEmpty() || message == lastProgress) return;
        lastProgress = message;
        QTextStream(stderr) << message << Qt::endl;
    };
    request.platformFinished = [](const VNX::ProjectPackBuildService::PlatformResult& platform) {
        for (const auto& warning : platform.result.warnings) QTextStream(stderr) << warning << Qt::endl;
        if (!platform.result.success) QTextStream(stderr) << platform.result.error << Qt::endl;
        for (const auto& record : platform.records) {
            QTextStream(stderr) << '[' << platform.definition.name << "] " << record.packName
                << ": " << (record.omitted ? "Omitted" : record.skipped ? "Up to date" : record.success ? "Built" : "Failed")
                << Qt::endl;
        }
    };
    const auto built = VNX::ProjectPackBuildService::run(request);
    for (const auto& warning : built.finalizationResult.warnings) QTextStream(stderr) << warning << Qt::endl;
    QJsonArray platformResults;
    QString firstError = built.preflightError;
    for (const auto& platform : built.platforms) {
        QJsonArray records;
        for (const auto& record : platform.records) {
            records.append(QJsonObject{
                {QStringLiteral("pack_name"), record.packName},
                {QStringLiteral("output_path"), record.outputPath},
                {QStringLiteral("language"), record.language},
                {QStringLiteral("success"), record.success},
                {QStringLiteral("skipped"), record.skipped},
                {QStringLiteral("omitted"), record.omitted},
                {QStringLiteral("file_size"), record.fileSize},
                {QStringLiteral("error"), record.error},
                {QStringLiteral("warnings"), QJsonArray::fromStringList(record.warnings)}});
        }
        const auto& result = platform.result;
        platformResults.append(QJsonObject{
            {QStringLiteral("platform_id"), platform.platform},
            {QStringLiteral("platform_name"), platform.definition.name},
            {QStringLiteral("output_path"), platform.outputDir},
            {QStringLiteral("success"), result.success},
            {QStringLiteral("canceled"), result.canceled},
            {QStringLiteral("error"), result.error},
            {QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings)},
            {QStringLiteral("built_pack_count"), result.builtPackCount},
            {QStringLiteral("skipped_pack_count"), result.skippedPackCount},
            {QStringLiteral("removed_pack_count"), result.removedPackCount},
            {QStringLiteral("media_cache_hits"), result.mediaCacheHits},
            {QStringLiteral("media_conversions"), result.mediaConversions},
            {QStringLiteral("outputs"), result.outputs},
            {QStringLiteral("records"), records}});
        if (!result.success && firstError.isEmpty()) firstError = result.error;
    }
    if (firstError.isEmpty()) firstError = built.finalizationResult.error;
    const bool wasCanceled = built.canceled || canceled();
    const bool finalized = built.finalization == VNX::ProjectPackBuildService::Finalization::Complete;
    const bool success = !wasCanceled && built.preflightSucceeded && built.allPlatformsSucceeded
        && finalized && built.finalizationResult.success;
    QJsonObject result{
        {QStringLiteral("success"), success},
        {QStringLiteral("output_path"), outputRoot},
        {QStringLiteral("canceled"), wasCanceled},
        {QStringLiteral("preflight_succeeded"), built.preflightSucceeded},
        {QStringLiteral("succeeded_platforms"), QJsonArray::fromStringList(built.succeededPlatforms)},
        {QStringLiteral("platforms"), platformResults},
        {QStringLiteral("finalization"), QJsonObject{
            {QStringLiteral("attempted"), finalized},
            {QStringLiteral("success"), built.finalizationResult.success},
            {QStringLiteral("error"), built.finalizationResult.error},
            {QStringLiteral("warnings"), QJsonArray::fromStringList(built.finalizationResult.warnings)}}}};
    QJsonObject response;
    if (success) {
        response.insert(QStringLiteral("status"), QStringLiteral("ok"));
    } else if (wasCanceled) {
        response = failure(QStringLiteral("CANCELED"), QStringLiteral("Canceled."));
    } else {
        response = failure(built.preflightSucceeded ? QStringLiteral("BUILD_FAILED")
                                                   : QStringLiteral("PACK_BUILD_PREFLIGHT_FAILED"), firstError);
    }
    response.insert(QStringLiteral("result"), result);
    return response;
}

}
