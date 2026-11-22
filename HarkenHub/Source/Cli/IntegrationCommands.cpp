#include "Cli/HubCli.h"

#include "Integrations/EngineTargetProbe.h"
#include "Integrations/EngineTargetStore.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/IntegrationStore.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Launch/ProjectOpenFlow.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>
#include <algorithm>

namespace harken::hub::Cli {
namespace {

QJsonObject relationship(const IntegrationRecord& record)
{
    return {{"id", record.integrationId}, {"project", record.harkenProjectPath}, {"targetId", record.engineTargetId},
        {"source", record.integrationSourcePath}, {"enabled", record.enabled}};
}

QJsonObject unityJson(const UnityIntegrationResult& result)
{
    QJsonArray steps;
    for (const auto& step : result.steps)
        steps.append(QJsonObject{{"id", step.id}, {"status", int(step.status)}, {"summary", step.summary}, {"cause", step.cause},
            {"remediation", step.remediation}, {"details", QJsonArray::fromStringList(step.details)}, {"artifacts", QJsonArray::fromStringList(step.artifacts)}});
    return {{"steps", steps}, {"location", result.integrationInstallPath}, {"installedVersion", result.installedVersion},
        {"sourceVersion", result.sourceVersion}, {"previousVersionAvailable", result.previousVersionAvailable},
        {"report", result.jsonReportPath}};
}

Reply unity(const Arguments& args, EngineTargetRecord& target)
{
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.restorePrevious = args.operation == "restore";
    const auto discovery = harken::hub::unity::UnityInstallation::discover(target.canonicalRoot);
    if (!request.restorePrevious) require(discovery.errors.isEmpty(), discovery.errors.join('\n'));
    if (target.integrationInstallPath.isEmpty() && discovery.installations.size() == 1)
        target.integrationInstallPath = QDir(target.canonicalRoot).relativeFilePath(discovery.installations.front().path);
    if (args.options.contains("location") && args.operation == "locate") {
        QString resolved;
        QString error;
        require(harken::hub::unity::UnityInstallation::resolveTarget(target.canonicalRoot, args.required("location"), &resolved, &error), error);
        require(discovery.installations.size() == 1 && harken::hub::unity::UnityInstallation::samePath(resolved, discovery.installations.front().path),
            "The supplied location does not identify the installed integration");
    }
    request.integrationInstallPath = target.integrationInstallPath;
    request.integrationSourcePath = args.operation == "install" ? args.required("source") : args.value("source");
    request.inspectOnly = args.operation == "locate";
    request.removeIntegration = args.operation == "remove";
    request.checkOnly = true;
    auto result = HubUnityIntegrationService::run(request);
    if (request.inspectOnly) {
        target.integrationInstallPath = result.integrationInstallPath;
        return {result.success ? 0 : 3, {{"ok", result.success}, {"data", unityJson(result)}}};
    }
    if (!result.success) return {3, {{"ok", false}, {"data", unityJson(result)}}};
    auto plan = unityJson(result);
    plan.remove("report");
    plan.remove("steps");
    confirm(args, plan);
    request.checkOnly = false;
    result = HubUnityIntegrationService::run(request);
    if (!result.integrationRemoved) target.integrationInstallPath = result.integrationInstallPath;
    return {result.success ? 0 : 3, {{"ok", result.success}, {"data", unityJson(result)}}};
}


}

Reply integrations(const Arguments& input)
{
    auto args = input;
    if (args.options.contains("source"))
        args.options["source"] = QFileInfo(args.required("source")).absoluteFilePath();
    EngineTargetStore targets;
    IntegrationStore associations;
    requireStore(targets, targets.open(userDataDir() + "/engine-targets.db"));
    requireStore(associations, associations.open(userDataDir() + "/integrations.db"));
    const auto registered = targets.list();
    requireStore(targets, targets.lastError().isEmpty());
    const auto relationships = associations.list();
    requireStore(associations, associations.lastError().isEmpty());
    if (args.operation == "list") {
        QJsonArray entries;
        QJsonArray links;
        for (const auto& target : registered) {
            if (target.engineKind != EngineKind::Unity) continue;
            const auto status = EngineTargetProbe::observe(target);
            entries.append(QJsonObject{{"id", target.targetId}, {"engine", "unity"},
                {"path", target.canonicalRoot}, {"name", target.displayName}, {"location", status.integrationInstallPath},
                {"version", status.integrationVersion}, {"status", status.status}, {"error", status.error}});
        }
        for (const auto& record : relationships) links.append(relationship(record));
        return success({{"targets", entries}, {"associations", links}});
    }
    if (args.operation == "unlink") {
        const auto id = args.required("id");
        const auto found = std::find_if(relationships.begin(), relationships.end(), [&](const auto& record) { return record.integrationId == id; });
        require(found != relationships.end(), "Unknown integration association");
        confirm(args, {{"association", relationship(*found)}, {"removeIntegrationFiles", false}});
        requireStore(associations, associations.remove(id));
        return success({{"id", id}});
    }
    const auto engine = args.value("engine", "unity");
    require(engine == "unity", "--engine must be unity", "InvalidArguments");
    EngineTargetRecord target;
    QString error;
    require(EngineTargetProbe::resolve(EngineKind::Unity, args.required("path"), &target, &error), error);
    target.addedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    bool known = false;
    for (const auto& current : registered)
        if (
#ifdef Q_OS_WIN
            current.canonicalRoot.compare(target.canonicalRoot, Qt::CaseInsensitive) == 0
#else
            current.canonicalRoot == target.canonicalRoot
#endif
            && current.engineKind == target.engineKind) {
            target = current;
            known = true;
            break;
        }
    if (args.options.contains("location")) target.integrationInstallPath = args.required("location");
    if (args.operation == "add-target") {
        if (!known) requireStore(targets, targets.add(target));
        return success({{"id", target.targetId}, {"path", target.canonicalRoot}});
    }
    if (args.operation == "link") {
        const auto project = ProjectProbe::probe(args.required("project"));
        require(project.ok, project.error);
        QJsonArray existing;
        for (const auto& record : relationships)
            if (record.engineTargetId == target.targetId || record.harkenProjectPath == project.canonicalPath) existing.append(relationship(record));
        confirm(args, {{"project", project.canonicalPath}, {"engine", engine}, {"target", target.canonicalRoot}, {"existing", existing}});
        require(existing.isEmpty(), "Unlink the existing association before linking these projects");
        if (!known) requireStore(targets, targets.add(target));
        ProjectStore projects;
        requireStore(projects, projects.open(userDataDir() + "/projects.db"));
        QString canonical;
        require(ProjectOpenFlow::ensureRegistered(&projects, project.canonicalPath, &canonical, &error), error);
        IntegrationRecord record;
        record.integrationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        record.engineTargetId = target.targetId;
        record.harkenProjectPath = canonical;
        record.integrationSourcePath = args.value("source");
        record.createdAt = record.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        requireStore(associations, associations.add(record, false));
        return success(relationship(record));
    }
    auto result = unity(args, target);
    if (result.exitCode == 0) {
        if (!known) requireStore(targets, targets.add(target));
        else requireStore(targets, targets.updateIntegrationInstallPath(target.targetId, target.integrationInstallPath));
        if (args.operation == "install")
            for (const auto& record : relationships)
                if (record.engineTargetId == target.targetId)
                    requireStore(associations, associations.updateIntegrationSourcePath(record.integrationId, args.required("source")));
    }
    return result;
}

}
