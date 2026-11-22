#include "Cli/HubCli.h"

#include "Launch/LaunchPreflight.h"
#include "Launch/ProjectCreateFlow.h"
#include "Launch/ProjectOpenFlow.h"
#include "Launch/StudioLauncher.h"
#include "Maintenance/BuildPinRegistry.h"
#include "Transactions/InstallIndex.h"
#include "Versions/IntegrityChecker.h"
#include "Versions/LocalBuildRegistry.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectIntakeConsumer.h"
#include "Projects/ProjectStore.h"
#include "Projects/ProjectVersionSelection.h"
#include "Versions/StudioInterface.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>

namespace harken::hub::Cli {
namespace {

InstalledVersion selectedVersion(const QString& build)
{
    const auto available = VersionScanner::scanAvailable();
    const auto selected = std::find_if(available.begin(), available.end(), [&](const auto& version) { return version.buildId == build; });
    require(selected != available.end(), "No available Studio version has build id " + build, "VersionUnavailable");
    return *selected;
}

void requireUnchangedStudio(const InstalledVersion& version)
{
    if (version.isLocalBuild()) {
        require(!version.executableFingerprint.isEmpty()
                && LocalBuildRegistry::fileFingerprint(version.executablePath) == version.executableFingerprint,
            "This local build changed and must be checked again.", "VersionUnavailable");
        return;
    }
    const auto index = InstallIndex::snapshot();
    require(index.ok, index.error);
    const auto record = std::find_if(index.records.begin(), index.records.end(),
        [&](const auto& item) { return item.buildId == version.buildId; });
    require(record != index.records.end(), "The selected version has no trusted installation record");
    const auto integrity = IntegrityChecker::check(record->installPath, record->packageManifestHash);
    require(integrity.ok, integrity.error, "VersionNeedsRepair");
}

QJsonObject projectJson(const ProjectRecord& project)
{
    const auto probe = ProjectProbe::probe(project.canonicalPath);
    return {{"path", project.canonicalPath},
        {"name", probe.ok ? probe.displayName : QFileInfo(project.canonicalPath).completeBaseName()},
        {"available", probe.ok}, {"error", probe.error}, {"preferredBuild", project.preferredStudioBuildId},
        {"lastOpenedAt", project.lastOpenedAt}};
}

}

Reply projects(const Arguments& args)
{
    if (args.operation == "templates") {
        const auto version = selectedVersion(args.required("build"));
        requireUnchangedStudio(version);
        QString error;
        const auto templates = StudioInterface::listTemplates(version, &error);
        require(templates.has_value(), error);
        QJsonArray entries;
        for (const auto& item : *templates)
            entries.append(QJsonObject{{"id", item.id}, {"name", item.displayName}, {"description", item.description}});
        return success({{"templates", entries}});
    }
    if (args.operation == "create") {
        requireConsent("studio");
        ProjectCreateRequest request;
        request.version = selectedVersion(args.required("build"));
        requireUnchangedStudio(request.version);
        request.name = args.required("name").trimmed();
        request.location = QFileInfo(args.required("location")).absoluteFilePath();
        require(QFileInfo(request.location).isDir(), "The project location must be an existing directory", "InvalidArguments");
        request.templateId = args.value("template");
        require(!request.name.contains(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]")) && request.name != "." && request.name != ".." && !request.name.endsWith('.'), "Invalid project name", "InvalidArguments");
        require(!QFileInfo::exists(QDir(request.location).filePath(request.name)), "The project directory already exists");
        QString error;
        if (!request.templateId.isEmpty()) {
            const auto templates = StudioInterface::listTemplates(request.version, &error);
            require(templates.has_value(), error);
            require(std::any_of(templates->begin(), templates->end(), [&](const auto& entry) { return entry.id == request.templateId; }),
                "Unknown template id; use projects templates to list the templates of the selected Studio", "InvalidArguments");
        }
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
