#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectLocator.h"
#include "Tooling/Harkenizer/Core/ProjectLoader.h"
#include "Tooling/Harkenizer/Core/ProjectPackBuildService.h"
#include "Core/SchemaArchive.h"
#include "ResourceWorker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

#include <csignal>
#include <cstring>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void interrupt(int)
{
    interrupted = 1;
}

bool canceled()
{
    return interrupted != 0;
}

int error(const QString& message, int code = 1)
{
    QTextStream(stderr) << message << Qt::endl;
    return code;
}

QString progressMessage(const VNX::PackBuildObservation& observation, const VNX::ProjectData& project)
{
    const auto* platform = VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(observation.platform));
    const QString prefix = platform ? QStringLiteral("[%1] ").arg(platform->name) : QString();
    const QString unit = observation.language.isEmpty() ? observation.packName
        : observation.language + QLatin1Char('/') + observation.packName;
    switch (observation.stage) {
    case VNX::PackBuildObservationStage::Preflight:
        return QStringLiteral("Checking outputs");
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
    case VNX::PackBuildObservationStage::Publishing:
        return prefix + QStringLiteral("Validating and publishing");
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

int main(int argc, char* argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--resource-worker") == 0)
        return Harkenizer::runResourceWorker();
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Harkenizer"));
    app.setOrganizationName(QStringLiteral("HarkenSound"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Build published packs from a saved Harken project."));
    const auto help = parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("list-targets or generate-packs"));
    parser.addPositionalArgument(QStringLiteral("project.hsp"), QStringLiteral("Saved project file"));
    parser.addOption({QStringLiteral("json"), QStringLiteral("Print target identities as JSON")});
    parser.addOption({QStringLiteral("all"), QStringLiteral("Export every Action for the requested platforms")});
    parser.addOption({QStringLiteral("platform-id"), QStringLiteral("Project platform UUID; may be repeated"), QStringLiteral("UUID")});
    parser.addOption({QStringLiteral("self-clean"), QStringLiteral("Remove outputs outside this export: on or off"), QStringLiteral("on|off")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("Output directory; relative paths use the project directory"), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("force"), QStringLiteral("Force re-export")});
    if (!parser.parse(app.arguments())) return error(parser.errorText(), 2);
    if (parser.isSet(help)) {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 2)
        return error(QStringLiteral("Specify a command and a saved .hsp project. Use --help for usage."), 2);
    const bool listing = positional.front() == QStringLiteral("list-targets");
    if (!listing && positional.front() != QStringLiteral("generate-packs"))
        return error(QStringLiteral("Unknown command: %1").arg(positional.front()), 2);
    if (listing) {
        if (!parser.isSet(QStringLiteral("json")) || parser.isSet(QStringLiteral("all"))
            || parser.isSet(QStringLiteral("platform-id")) || parser.isSet(QStringLiteral("self-clean"))
            || parser.isSet(QStringLiteral("output")) || parser.isSet(QStringLiteral("force")))
            return error(QStringLiteral("Use list-targets <project.hsp> --json."), 2);
    } else if (parser.isSet(QStringLiteral("json")) || !parser.isSet(QStringLiteral("all"))
        || parser.values(QStringLiteral("platform-id")).isEmpty()
        || parser.values(QStringLiteral("self-clean")).size() != 1
        || (parser.value(QStringLiteral("self-clean")) != QStringLiteral("on")
            && parser.value(QStringLiteral("self-clean")) != QStringLiteral("off"))
        || parser.values(QStringLiteral("output")).size() > 1
        || (parser.isSet(QStringLiteral("output")) && parser.value(QStringLiteral("output")).trimmed().isEmpty())) {
        return error(QStringLiteral("Use generate-packs <project.hsp> --all --platform-id <UUID> --self-clean <on|off> [--output <directory>] [--force]."), 2);
    }
    QStringList platforms;
    for (const QString& value : parser.values(QStringLiteral("platform-id"))) {
        const auto id = VNX::ProjectPlatformSemantics::parseId(value);
        if (!id) return error(QStringLiteral("Invalid project platform UUID: %1").arg(value), 2);
        const QString canonical = VNX::ProjectPlatformSemantics::idString(*id);
        if (platforms.contains(canonical)) return error(QStringLiteral("A platform was requested more than once: %1").arg(value), 2);
        platforms.append(canonical);
    }
    const QFileInfo projectFile(positional.at(1));
    if (!projectFile.isFile() || projectFile.suffix().compare(QStringLiteral("hsp"), Qt::CaseInsensitive) != 0)
        return error(QStringLiteral("Project must be an existing .hsp file: %1").arg(positional.at(1)));
    const auto location = VNX::ProjectLocator::locate(projectFile.absoluteFilePath());
    if (!location.ok()) return error(VNX::projectIssueCode(location.issue));
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    const auto instance = VNX::ProjectInstance::acquire(location.projectFilePath, VNX::ProjectInstance::ActivationPolicy::Deny);
    if (instance.status != VNX::ProjectInstance::Status::Acquired) return error(instance.error);
    QString schemaError;
    if (!VNX::SchemaArchive::instance().verifyBuildSnapshot(&schemaError)) return error(schemaError);
    auto prepared = VNX::ProjectLoader::prepareProject(location.projectFilePath, false, {}, {}, canceled,
        nullptr, VNX::ProjectSerializer::LoadMode::ReadOnly);
    if (canceled()) return error(QStringLiteral("Canceled."), 130);
    if (!prepared.project) return error(prepared.error);
    for (const auto& diagnostic : prepared.diagnostics) QTextStream(stderr) << diagnostic << Qt::endl;
    if (listing) {
        QTextStream(stdout) << QJsonDocument(listTargets(*prepared.project)).toJson(QJsonDocument::Indented);
        return 0;
    }
    auto& project = *prepared.project;
    for (const auto& platform : platforms) {
        if (!VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(platform)))
            return error(QStringLiteral("Unknown project platform UUID: %1").arg(platform));
    }
    const QString projectRoot = QFileInfo(project.projectPath).absolutePath();
    auto& assets = VNX::AssetRegistry::instance();
    assets.replaceSnapshot(projectRoot, prepared.assets);
    assets.setPlatforms(project.platforms);
    QString snapshotError;
    const auto snapshot = VNX::ProjectBuildSnapshot::capture(project, &snapshotError);
    if (!snapshot) return error(snapshotError);
    const QString outputRoot = parser.isSet(QStringLiteral("output"))
        ? VNX::PathUtils::toAbsolutePath(parser.value(QStringLiteral("output")), projectRoot)
        : VNX::PathUtils::packBuildOutputRoot(project.projectPath, project.packBuildOutputDir);
    if (VNX::PathUtils::isProtectedPackOutputPath(outputRoot, project.projectPath))
        return error(VNX::PathUtils::packBuildOutputLocationError());
    const QFileInfo outputInfo(outputRoot);
    if (outputInfo.isFile() || outputInfo.fileName().endsWith(QStringLiteral(".bytes"), Qt::CaseInsensitive)
        || outputInfo.fileName().endsWith(QStringLiteral(".pack"), Qt::CaseInsensitive))
        return error(QStringLiteral("Output must be a directory, not a pack file."));
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
    if (!request.outputLock) return error(lockError);
    const auto readiness = VNX::ProjectPackExporter::ValidatePackBuildReadiness(snapshot->project, outputRoot);
    for (const auto& issue : readiness.issues)
        QTextStream(stderr) << issue.message << Qt::endl;
    if (readiness.errorCount > 0) return 1;
    QString lastProgress;
    request.options.observationCallback = [&lastProgress, &project](const VNX::PackBuildObservation& observation) {
        const auto message = progressMessage(observation, project);
        if (message.isEmpty() || message == lastProgress) return;
        lastProgress = message;
        QTextStream(stdout) << message << Qt::endl;
    };
    request.platformFinished = [](const VNX::ProjectPackBuildService::PlatformResult& platform) {
        for (const auto& warning : platform.result.warnings) QTextStream(stderr) << warning << Qt::endl;
        if (!platform.result.success) QTextStream(stderr) << platform.result.error << Qt::endl;
        for (const auto& record : platform.records) {
            QTextStream(stdout) << '[' << platform.definition.name << "] " << record.packName
                << ": " << (record.omitted ? "Omitted" : record.skipped ? "Up to date" : record.success ? "Built" : "Failed")
                << Qt::endl;
        }
    };
    const auto result = VNX::ProjectPackBuildService::run(request);
    for (const auto& warning : result.finalizationResult.warnings) QTextStream(stderr) << warning << Qt::endl;
    if (result.canceled || canceled()) return error(QStringLiteral("Canceled."), 130);
    if (!result.preflightSucceeded) return error(result.preflightError);
    if (!result.allPlatformsSucceeded || result.finalization != VNX::ProjectPackBuildService::Finalization::Complete
        || !result.finalizationResult.success) {
        if (!result.finalizationResult.error.isEmpty()) return error(result.finalizationResult.error);
        return 1;
    }
    QTextStream(stdout) << "Export complete: " << outputRoot << Qt::endl;
    return 0;
}
