#include "Cli/HubCli.h"

#include "Integrations/EngineTargetProbe.h"
#include "Integrations/EngineTargetStore.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/IntegrationStore.h"
#include "Integrations/OfficialUnitySource.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Launch/ProjectOpenFlow.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"
#include "Security/TrustAnchors.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>

namespace VNH::Cli {
namespace {

QJsonObject relationship(const IntegrationRecord& record)
{
    return {{"id", record.integrationId}, {"project", record.harkenProjectPath}, {"targetId", record.engineTargetId},
        {"source", record.integrationSourcePath}, {"enabled", record.enabled}};
}

VNX::UnityInstallation::ContentAction contentAction(const QString& value)
{
    using Action = VNX::UnityInstallation::ContentAction;
    if (value == "keep") return Action::Keep;
    if (value == "update") return Action::Update;
    if (value == "remove") return Action::Remove;
    require(value == "ask", "Content actions must be ask, keep, update or remove", "InvalidArguments");
    return Action::Ask;
}

QJsonObject unityJson(const UnityIntegrationResult& result)
{
    QJsonArray steps;
    for (const auto& step : result.steps)
        steps.append(QJsonObject{{"id", step.id}, {"status", int(step.status)}, {"summary", step.summary}, {"cause", step.cause},
            {"remediation", step.remediation}, {"details", QJsonArray::fromStringList(step.details)}, {"artifacts", QJsonArray::fromStringList(step.artifacts)}});
    QJsonArray files;
    for (const auto& file : result.fileChanges) files.append(QJsonObject{{"action", file.action}, {"path", file.path}});
    return {{"steps", steps}, {"files", files}, {"location", result.integrationInstallPath},
        {"current", result.targetAlreadyCurrent}, {"documentationPresent", result.documentationPresent},
        {"samplesPresent", result.samplesPresent}, {"contentSelectionRequired", result.requiresContentSelection},
        {"contentBackup", result.contentBackupPath}, {"removalBackup", result.removalBackupPath},
        {"restorationBackup", result.restorationBackupPath}, {"migrationBackup", result.migrationBackupPath},
        {"migrationDestination", result.migrationTargetPath}, {"report", result.jsonReportPath},
        {"conflicts", QJsonArray::fromStringList(result.conflictingFiles)}};
}

Reply unity(const Arguments& args, EngineTargetRecord& target)
{
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.integrationInstallPath = target.integrationInstallPath;
    request.integrationSourcePath = args.value("source");
    request.documentation = contentAction(args.value("documentation", "ask"));
    request.samples = contentAction(args.value("samples", "ask"));
    request.preserveUnownedTarget = args.flag("preserve-unowned");
    request.checkOnly = true;
    request.verifyInstalledOnly = args.operation == "verify" || args.operation == "locate";
    request.removeIntegration = args.operation == "remove";
    request.migrateIntegration = args.operation == "move" || args.operation == "recover-move";
    request.recoverMigration = args.operation == "recover-move";
    request.checkOnly = !request.migrateIntegration;
    request.restoreIntegration = args.operation == "restore" || args.operation == "recover-restore";
    request.recoverRestore = args.operation == "recover-restore";
    if (args.operation == "install") request.integrationSourcePath = args.required("source");
    if (request.removeIntegration) request.removalBackupPath = args.required("backup");
    if (request.migrateIntegration) {
        request.migrationTargetPath = args.required("destination");
        request.migrationBackupPath = args.required("backup");
    }
    OfficialUnitySourceResult source;
    if (request.restoreIntegration && !request.recoverRestore) {
        const auto discovery = VNX::UnityInstallation::discover(target.canonicalRoot);
        require(discovery.errors.isEmpty() && discovery.installations.size() == 1, "A single managed integration must be identified before restoration");
        const auto receipt = QJsonDocument::fromJson(readFile(discovery.installations.front().path + "/.harken-integration-state.json")).object();
        OfficialUnitySourceRequest selection;
        selection.buildId = args.value("build", receipt["source_release"].toObject()["buildId"].toString());
        require(!selection.buildId.isEmpty(), "The original build could not be identified. Supply --build.");
        selection.packageVersion = receipt["package_version"].toString();
        selection.localSource = args.value("source");
        source = OfficialUnitySource::prepare(selection, TrustAnchors::manifestPublicKey());
        require(source.ok, source.error);
        request.integrationSourcePath = source.sourcePath;
        request.restorationBackupPath = args.required("backup");
        request.confirmUnownedConflicts = args.flag("accept-conflicts");
        request.officialRelease = {{"buildId", source.release.buildId}, {"version", source.release.version},
            {"channel", source.release.channel}, {"platform", source.release.platform}, {"architecture", source.release.architecture},
            {"packageSha256", source.release.sha256Hex}, {"packageManifestSha256", source.inventoryHash}};
        const QString prefix = source.inventory.unityRoot + '/';
        for (auto it = source.inventory.files.begin(); it != source.inventory.files.end(); ++it)
            if (it.key().startsWith(prefix)) request.officialFileHashes[it.key().mid(prefix.size())] = it->sha256.toLatin1();
    }
    auto result = HubUnityIntegrationService::run(request);
    if (request.verifyInstalledOnly) {
        target.integrationInstallPath = result.integrationInstallPath;
        return {result.success ? 0 : 3, {{"ok", result.success}, {"data", unityJson(result)}}};
    }
    if (result.requiresContentSelection)
        return {4, {{"ok", false}, {"contentSelectionRequired", true}, {"data", unityJson(result)},
            {"message", "Choose --documentation and --samples actions for existing components, then review again."}}};
    const bool failed = std::any_of(result.steps.begin(), result.steps.end(), [](const auto& step) { return step.status == UnityIntegrationStepStatus::Failed; });
    if (failed || (!result.success && !result.requiresConfirmation))
        return {3, {{"ok", false}, {"data", unityJson(result)}}};
    auto plan = unityJson(result);
    plan.remove("report");
    plan.remove("steps");
    plan["fingerprint"] = QString::fromLatin1(result.planHash.toHex());
    plan["migrationFingerprint"] = result.migrationPlanHash;
    confirm(args, plan);
    request.checkOnly = false;
    request.confirmTargetChanges = true;
    request.confirmRemoval = true;
    request.confirmMigration = true;
    request.approvedPlanHash = result.planHash;
    request.migrationPlanHash = result.migrationPlanHash;
    result = HubUnityIntegrationService::run(request);
    target.integrationInstallPath = result.integrationMigrated ? result.migrationTargetPath : result.integrationInstallPath;
    return {result.success ? 0 : result.requiresConfirmation ? 4 : 3, {{"ok", result.success}, {"data", unityJson(result)}}};
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
        if (current.canonicalRoot.compare(target.canonicalRoot, Qt::CaseInsensitive) == 0 && current.engineKind == target.engineKind) {
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
    if (result.exitCode == 0 && args.operation != "verify") {
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
