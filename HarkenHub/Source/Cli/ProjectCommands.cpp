#include "Cli/HubCli.h"

#include "Launch/LaunchPreflight.h"
#include "Launch/ProjectCreateFlow.h"
#include "Launch/ProjectOpenFlow.h"
#include "Launch/StudioLauncher.h"
#include "Maintenance/BuildPinRegistry.h"
#include "Transactions/InstallIndex.h"
#include "Versions/IntegrityChecker.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectIntakeConsumer.h"
#include "Projects/ProjectStore.h"
#include "Projects/ProjectVersionSelection.h"
#include "Templates/ProjectTemplateCatalog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>

namespace VNH::Cli {
namespace {

InstalledVersion selectedVersion(const QString& build)
{
    const auto available = VersionScanner::scanAvailable();
    const auto selected = std::find_if(available.begin(), available.end(), [&](const auto& version) { return version.buildId == build; });
    require(selected != available.end(), "No available Studio version has build id " + build, "VersionUnavailable");
    return *selected;
}

QJsonObject projectJson(const ProjectRecord& project)
{
    const auto probe = ProjectProbe::probe(project.canonicalPath);
    return {{"id", project.projectId}, {"path", project.canonicalPath},
        {"name", probe.ok ? probe.displayName : QFileInfo(project.canonicalPath).completeBaseName()},
        {"available", probe.ok}, {"error", probe.error}, {"preferredBuild", project.preferredStudioBuildId},
        {"lastOpenedAt", project.lastOpenedAt}, {"schema", probe.schemaVersion}};
}

}

Reply projects(const Arguments& args)
{
    if (args.operation == "templates") {
        const auto version = selectedVersion(args.required("build"));
        QJsonArray entries;
        for (const auto& item : ProjectTemplateCatalog::listAvailable(QFileInfo(version.executablePath).absolutePath()))
            entries.append(QJsonObject{{"id", item.id}, {"name", item.displayName}, {"description", item.description}, {"path", item.filePath}, {"userTemplate", item.userTemplate}, {"content", item.content}});
        return success({{"templates", entries}, {"userTemplateDirectory", ProjectTemplateCatalog::userTemplateDir()}});
    }
    if (args.operation == "import-template") {
        const QString path = QFileInfo(args.required("path")).absoluteFilePath();
        const auto validation = ProjectTemplateCatalog::validateFile(path);
        require(validation.valid, validation.toUserText());
        confirm(args, {{"source", path}, {"sha256", fileHash(path)}, {"destination", ProjectTemplateCatalog::userTemplateDir()}});
        TemplateValidationResult result;
        const auto imported = ProjectTemplateCatalog::importFromFile(path, &result);
        require(imported.has_value(), result.toUserText());
        return success({{"id", imported->id}, {"path", imported->filePath}});
    }
    if (args.operation == "create") {
        requireConsent("studio");
        ProjectCreateRequest request;
        request.version = selectedVersion(args.required("build"));
        if (!request.version.isLocalBuild()) {
            const auto index = InstallIndex::snapshot();
            require(index.ok, index.error);
            const auto record = std::find_if(index.records.begin(), index.records.end(),
                [&](const auto& item) { return item.buildId == request.version.buildId; });
            require(record != index.records.end(), "The selected version has no trusted installation record");
            const auto integrity = IntegrityChecker::check(record->installPath, record->packageManifestHash, record->capabilitiesHash);
            require(integrity.ok, integrity.error, "VersionNeedsRepair");
        }
        request.name = args.required("name").trimmed();
        request.location = QFileInfo(args.required("location")).absoluteFilePath();
        require(QFileInfo(request.location).isDir(), "The project location must be an existing directory", "InvalidArguments");
        request.templateId = args.value("template");
        request.templatePath = args.value("template-file");
        require(!request.name.contains(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]")) && request.name != "." && request.name != ".." && !request.name.endsWith('.'), "Invalid project name", "InvalidArguments");
        require(!QFileInfo::exists(QDir(request.location).filePath(request.name)), "The project directory already exists");
        require(request.templateId.isEmpty() || request.templatePath.isEmpty(), "Use either --template or --template-file", "InvalidArguments");
        if (!request.templateId.isEmpty()) {
            const auto templates = ProjectTemplateCatalog::listAvailable(QFileInfo(request.version.executablePath).absolutePath());
            const auto matches = std::count_if(templates.begin(), templates.end(), [&](const auto& entry) { return entry.id == request.templateId; });
            require(matches == 1, matches == 0 ? "Unknown template id; use projects templates to list available templates"
                : "The template id is ambiguous; use --template-file with the desired template path", "InvalidArguments");
            request.templatePath = std::find_if(templates.begin(), templates.end(), [&](const auto& entry) { return entry.id == request.templateId; })->filePath;
        }
        if (!request.templatePath.isEmpty()) {
            request.templatePath = QFileInfo(request.templatePath).absoluteFilePath();
            const auto validation = ProjectTemplateCatalog::validateFile(request.templatePath);
            require(validation.valid, validation.toUserText());
            if (request.templateId.isEmpty()) request.templateId = QFileInfo(request.templatePath).completeBaseName();
        }
        QString error;
        const QString projectPath = QDir(request.location).filePath(request.name + '/' + request.name + ".hsp");
        require(ProjectIntakeConsumer::rememberCreationBuild(projectPath, request.version.buildId, request.version.expectedBuildId(), &error), error);
        if (!ProjectCreateFlow::create(request, &error)) {
            ProjectIntakeConsumer::forgetCreationBuild(projectPath);
            require(false, error);
        }
        return success({{"started", true}, {"projectDirectory", QDir(request.location).filePath(request.name)}, {"buildId", request.version.buildId}});
    }
    ProjectStore store;
    requireStore(store, store.open(userDataDir() + "/projects.db"));
    QJsonArray warnings;
    const auto refreshPins = [&] {
        QString error;
        if (!BuildPinRegistry::refreshFromProjects(store, &error))
            warnings.append(QJsonObject{{"code", "BuildPinsNotRefreshed"}, {"message", error}});
    };
    const auto complete = [&](QJsonObject data) {
        if (!warnings.isEmpty()) data["warnings"] = warnings;
        return success(data);
    };
    ProjectIntakeConsumer intake(&store);
    if (intake.consumeAll()) refreshPins();
    if (args.operation == "list") {
        QJsonArray projects;
        for (const auto& project : store.listProjects()) projects.append(projectJson(project));
        requireStore(store, store.lastError().isEmpty());
        return complete({{"projects", projects}});
    }
    QString path = QDir::cleanPath(QFileInfo(args.required("path")).absoluteFilePath());
    if (args.operation == "add") {
        QString error;
        require(ProjectOpenFlow::ensureRegistered(&store, path, &path, &error), error);
        for (const auto& project : store.listProjects())
            if (project.canonicalPath == path) return complete(projectJson(project));
        requireStore(store, false);
    }
    const auto probe = ProjectProbe::probe(path);
    if (probe.ok) path = probe.canonicalPath;
    const auto records = store.listProjects();
    requireStore(store, store.lastError().isEmpty());
    const auto project = std::find_if(records.begin(), records.end(), [&](const auto& value) { return value.canonicalPath == path; });
    require(project != records.end(), "Project is not registered. Use projects add first.");
    if (args.operation == "remove") {
        confirm(args, {{"registration", projectJson(*project)}, {"deleteProjectFiles", false}});
        requireStore(store, store.removeProject(path));
    } else if (args.operation == "select-version") {
        const auto version = selectedVersion(args.required("build"));
        requireStore(store, store.setPreferredBuild(path, version.buildId));
    } else {
        requireConsent("studio");
        const auto available = VersionScanner::scanAvailable();
        const auto selection = ProjectVersionSelection::resolve(*project, probe, available, args.value("build"));
        require(selection.errorCode.isEmpty(), selection.reason, selection.errorCode);
        const auto decision = LaunchPreflight::evaluate(*project, available, InstallIndex::snapshot(), selection.buildId);
        requireStore(store, store.setLastPreflight(path, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)));
        require(decision.verdict == LaunchVerdict::Ready, decision.reason, decision.blockCode);
        QString error;
        require(StudioLauncher::start({decision.version.installPath, decision.version.executablePath, path}, &error), error);
        return complete({{"started", true}, {"path", path}, {"buildId", decision.version.buildId}});
    }
    refreshPins();
    return complete({{"path", path}, {"operation", args.operation}});
}

}
