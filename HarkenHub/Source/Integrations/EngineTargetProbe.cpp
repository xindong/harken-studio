#include "Integrations/EngineTargetProbe.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/Unity/UnityInstallation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace VNH {
namespace {

QString hubTranslate(const QString& source)
{
    const QByteArray utf8 = source.toUtf8();
    return QCoreApplication::translate("HarkenHub", utf8.constData());
}

QJsonObject jsonObject(const QString& path, bool* ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(),
                                                            &parseError);
    *ok = parseError.error == QJsonParseError::NoError
        && document.isObject();
    return *ok ? document.object() : QJsonObject{};
}

QString canonicalDirectory(const QString& path)
{
    const QFileInfo info(path);
    const QString value = info.canonicalFilePath();
    return QDir::cleanPath(value.isEmpty() ? info.absoluteFilePath() : value);
}

}

bool EngineTargetProbe::resolve(const QString& input, EngineTargetRecord* record,
                                QString* error)
{
    const QFileInfo selected(input);
    if (selected.isFile() && selected.suffix().compare(
            QStringLiteral("uproject"), Qt::CaseInsensitive) == 0)
        return resolve(EngineKind::Unreal, input, record, error);
    const QDir root(input);
    const bool unity = selected.isDir() && root.exists(QStringLiteral("Assets"))
        && root.exists(QStringLiteral("Packages"))
        && root.exists(QStringLiteral("ProjectSettings"));
    const bool unreal = selected.isDir()
        && !root.entryList({QStringLiteral("*.uproject")}, QDir::Files).isEmpty();
    if (unity && unreal) {
        if (error) *error = hubTranslate(QStringLiteral(
            "Multiple game projects were found. Select a separate Unity project folder."));
        return false;
    }
    if (unity || unreal)
        return resolve(unity ? EngineKind::Unity : EngineKind::Unreal, input, record, error);
    if (error) *error = hubTranslate(QStringLiteral(
        "Select a Unity project folder."));
    return false;
}

bool EngineTargetProbe::resolve(EngineKind kind, const QString& input,
                                EngineTargetRecord* record, QString* error)
{
    if (record == nullptr) {
        if (error != nullptr)
            *error = hubTranslate(QStringLiteral("Engine target output is unavailable"));
        return false;
    }
    QFileInfo selected(input);
    QString root;
    QString descriptor;
    if (kind == EngineKind::Unity) {
        root = selected.isDir() ? selected.absoluteFilePath()
                                : selected.absolutePath();
        QDir directory(root);
        if (!directory.exists(QStringLiteral("Assets"))
            || !directory.exists(QStringLiteral("Packages"))
            || !directory.exists(QStringLiteral("ProjectSettings"))) {
            if (error != nullptr)
                *error = hubTranslate(QStringLiteral("The selected directory is not a Unity project"));
            return false;
        }
    } else {
        if (selected.isFile()
            && selected.suffix().compare(QStringLiteral("uproject"),
                                         Qt::CaseInsensitive) == 0) {
            descriptor = selected.absoluteFilePath();
            root = selected.absolutePath();
        } else {
            QDir directory(selected.absoluteFilePath());
            const QStringList descriptors = directory.entryList(
                {QStringLiteral("*.uproject")}, QDir::Files, QDir::Name);
            if (descriptors.size() != 1) {
                if (error != nullptr)
                    *error = hubTranslate(QStringLiteral(
                        "The Unreal project directory must contain exactly one .uproject file"));
                return false;
            }
            root = directory.absolutePath();
            descriptor = directory.filePath(descriptors.front());
        }
    }
    root = canonicalDirectory(root);
    if (root.isEmpty()) {
        if (error != nullptr)
            *error = hubTranslate(QStringLiteral("The engine project path cannot be resolved"));
        return false;
    }
    record->targetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record->engineKind = kind;
    record->canonicalRoot = root;
    record->descriptorPath = descriptor.isEmpty()
        ? QString() : QFileInfo(descriptor).canonicalFilePath();
    record->displayName = QFileInfo(root).fileName();
    return true;
}

namespace {

EngineTargetObservation observeLayout(const EngineTargetRecord& record)
{
    EngineTargetObservation result;
    QDir root(record.canonicalRoot);
    if (!root.exists()) {
        result.health = EngineTargetHealth::Missing;
        result.error = hubTranslate(QStringLiteral("The game project directory is missing."));
        result.remediation = hubTranslate(QStringLiteral("Restore the project directory or select its current location. No SDK files can be changed until the project is available."));
        return result;
    }
    if (record.engineKind == EngineKind::Unity) {
        result.validTarget = root.exists(QStringLiteral("Assets"))
            && root.exists(QStringLiteral("Packages"))
            && root.exists(QStringLiteral("ProjectSettings"));
        if (!result.validTarget) {
            result.health = EngineTargetHealth::Unreadable;
            result.error = hubTranslate(QStringLiteral("Selected directory is not a Unity project root."));
            result.remediation = hubTranslate(QStringLiteral("Select the Unity project root containing Assets, Packages, and ProjectSettings."));
            return result;
        }
        const QString versionPath = root.filePath(
            QStringLiteral("ProjectSettings/ProjectVersion.txt"));
        QFile versionFile(versionPath);
        if (versionFile.open(QIODevice::ReadOnly)) {
            const QString text = QString::fromUtf8(versionFile.readAll()).trimmed();
            result.engineVersion = text.section(QStringLiteral("m_EditorVersion:"),
                                                1).trimmed();
        }
        if (VNX::UnityInstallation::hasPendingMigration(record.canonicalRoot)) {
            result.health = EngineTargetHealth::Incomplete;
            result.status = hubTranslate(QStringLiteral("Migration unfinished"));
            result.error = hubTranslate(QStringLiteral("An unfinished SDK migration must be recovered before continuing."));
            result.remediation = hubTranslate(QStringLiteral("Use Recover SDK Migration to restore a consistent installation before changing, moving or uninstalling the SDK."));
            result.canMoveSdk = true;
            return result;
        }
        const auto discovery = VNX::UnityInstallation::discover(record.canonicalRoot);
        if (discovery.installations.size() == 1)
            result.sdkInstallPath = root.relativeFilePath(discovery.installations.front().path);
        if (!discovery.errors.isEmpty() || discovery.installations.size() > 1) {
            result.health = EngineTargetHealth::Incomplete;
            result.error = discovery.errors.join(QLatin1Char('\n'));
            if (discovery.installations.size() > 1) {
                result.status = hubTranslate(QStringLiteral("Multiple SDK installations"));
                result.remediation = hubTranslate(QStringLiteral("Back up the project, identify the SDK copy used by the game, and remove the extra copies with their original integration tools. Check again when only one installation remains."));
                if (!result.error.isEmpty()) result.error += QLatin1Char('\n');
                result.error += hubTranslate(QStringLiteral("Multiple SDK installations were found:"));
                for (const auto& installation : discovery.installations)
                    result.error += QLatin1Char('\n') + installation.path;
            } else if (!discovery.sdkReferences.isEmpty()) {
                result.status = hubTranslate(QStringLiteral("Package Manager dependency"));
                result.remediation = hubTranslate(QStringLiteral("Manage this SDK dependency in Unity Package Manager. Before switching to a Hub installation, back up the project and remove the existing SDK dependency there, then check again."));
            } else {
                result.status = hubTranslate(QStringLiteral("SDK inspection blocked"));
                result.remediation = hubTranslate(QStringLiteral("Resolve the reported file access, package JSON or path errors, then check again. Hub must identify a single SDK location before changing its files."));
            }
            return result;
        }
        if (discovery.installations.isEmpty()) {
            result.health = EngineTargetHealth::NotInstalled;
            result.canChangeSdk = true;
            result.remediation = hubTranslate(QStringLiteral("No SDK is installed. Use Install SDK and select a complete SDK source. Moving and uninstalling require an existing installation."));
            return result;
        }
        const auto& installation = discovery.installations.front();
        const QString packageRoot = installation.path;
        bool packageOk = false;
        const QJsonObject package = jsonObject(
            QDir(packageRoot).filePath(QStringLiteral("package.json")), &packageOk);
        result.integrationVersion = package.value(QStringLiteral("version")).toString();
        QString validatedPath;
        result.sdkInstallPath = root.relativeFilePath(packageRoot);
        if (!VNX::UnityInstallation::resolveTarget(record.canonicalRoot, result.sdkInstallPath,
                                                  &validatedPath, &result.error)
            || !VNX::UnityInstallation::validateTree(packageRoot, &result.error)) {
            result.health = EngineTargetHealth::Incomplete;
            result.status = hubTranslate(QStringLiteral("Unsafe or unreadable SDK path"));
            result.remediation = hubTranslate(QStringLiteral("Resolve the reported path or permission problem. SDK folders must be real directories in the supported project locations, without symbolic links or junctions. Check again after correcting the path."));
            return result;
        }
        if (!installation.managed) {
            result.health = EngineTargetHealth::Unmanaged;
            const bool hasRecord = QFileInfo::exists(QDir(packageRoot).filePath(QStringLiteral(".harken-integration-state.json")));
            result.status = hubTranslate(hasRecord ? QStringLiteral("Installation record invalid")
                                                  : QStringLiteral("Installation record missing"));
            result.error = hubTranslate(QStringLiteral("The SDK ownership record is missing, invalid or unreadable. Restore it from backup before updating or removing the SDK."));
            result.remediation = hubTranslate(QStringLiteral("Restore this installation's ownership record from its own backup, then check again. Do not copy a record from another SDK or project. Without the record, Hub cannot distinguish SDK files from project-specific files; use the original installation tool if no backup exists."));
            return result;
        }
        bool manifestOk = false;
        const auto manifest = jsonObject(QDir(packageRoot).filePath(QStringLiteral("harken-integration.json")), &manifestOk);
        result.canChangeSdk = true;
        if (!packageOk || package.value(QStringLiteral("name")).toString() != QStringLiteral("com.harken.core")
            || !manifestOk || manifest.value(QStringLiteral("schema_version")).toInt() != 3
            || manifest.value(QStringLiteral("package_name")).toString() != QStringLiteral("com.harken.core")) {
            result.health = EngineTargetHealth::Incomplete;
            result.status = hubTranslate(QStringLiteral("SDK metadata needs repair"));
            result.error = hubTranslate(QStringLiteral("The installation record is available, but package.json or harken-integration.json is missing or invalid."));
            result.remediation = hubTranslate(QStringLiteral("Use Change SDK with a complete SDK source to repair the managed files. Move and uninstall require readable package metadata, so repair it first. Existing safety checks still run before applying any change."));
            return result;
        }
        result.health = EngineTargetHealth::Managed;
        result.canRemoveSdk = discovery.sdkReferences.isEmpty();
        result.canMoveSdk = result.canRemoveSdk;
        if (!result.canRemoveSdk) {
            result.dependencyRestricted = true;
            result.status = hubTranslate(QStringLiteral("Installed; move and uninstall restricted by dependencies"));
            result.error = discovery.sdkReferences.join(QLatin1Char('\n'));
            result.remediation = hubTranslate(QStringLiteral("The SDK is installed. Change SDK remains available. Before moving or uninstalling, resolve the SDK dependencies in Unity Package Manager and package manifests so Unity cannot load another referenced copy or lose a package reference."));
        } else {
            const auto dependencies = package.value(QStringLiteral("dependencies")).toObject();
            bool localDependency = false;
            for (auto it = dependencies.begin(); it != dependencies.end(); ++it)
                localDependency |= it.value().toString().startsWith(QStringLiteral("file:"));
            for (const auto& reference : discovery.localPackagePaths)
                localDependency |= VNX::UnityInstallation::containsPath(packageRoot, reference)
                    || VNX::UnityInstallation::containsPath(reference, packageRoot);
            if (localDependency) {
                result.canMoveSdk = false;
                result.dependencyRestricted = true;
                result.status = hubTranslate(QStringLiteral("Installed; move restricted by local dependencies"));
                result.remediation = hubTranslate(QStringLiteral("Change and uninstall remain available. Before moving the SDK, resolve local file: dependencies that belong to or reference this installation, then check again."));
            }
        }
        return result;
    }

    const QFileInfo descriptor(record.descriptorPath);
    result.validTarget = descriptor.isFile();
    if (!result.validTarget) {
        result.health = EngineTargetHealth::Missing;
        result.error = hubTranslate(QStringLiteral("The Unreal project descriptor is missing."));
        result.remediation = hubTranslate(QStringLiteral("Restore the .uproject file or select the current Unreal project location, then check again."));
        return result;
    }
    bool projectOk = false;
    const QJsonObject project = jsonObject(descriptor.absoluteFilePath(),
                                           &projectOk);
    if (!projectOk) {
        result.validTarget = false;
        result.health = EngineTargetHealth::Unreadable;
        result.error = hubTranslate(QStringLiteral("The Unreal project descriptor is not valid JSON."));
        result.remediation = hubTranslate(QStringLiteral("Restore or repair the .uproject file, then check again."));
        return result;
    }
    result.engineVersion = project.value(QStringLiteral("EngineAssociation"))
                               .toString();
    if (ProjectIntegrationExecutor::hasPendingUnrealMigration(record)) {
        result.health = EngineTargetHealth::Incomplete;
        result.status = hubTranslate(QStringLiteral("Migration unfinished"));
        result.error = hubTranslate(QStringLiteral("An unfinished SDK migration must be recovered before continuing."));
        result.remediation = hubTranslate(QStringLiteral("Use Recover SDK Migration to restore a consistent installation before changing, moving or uninstalling the SDK."));
        result.canMoveSdk = true;
        return result;
    }
    QString pluginRoot;
    if (!ProjectIntegrationExecutor::locateUnrealInstallation(record, &pluginRoot, &result.error)) {
        result.health = EngineTargetHealth::Incomplete;
        result.status = hubTranslate(QStringLiteral("SDK inspection blocked"));
        result.remediation = hubTranslate(QStringLiteral("Resolve the reported plugin locations or path access errors, then check again. Hub must identify one SDK installation before changing its files."));
        return result;
    }
    if (pluginRoot.isEmpty()) {
        result.health = EngineTargetHealth::NotInstalled;
        result.canChangeSdk = true;
        result.remediation = hubTranslate(QStringLiteral("No SDK is installed. Use Install SDK and select a complete SDK source. Moving and uninstalling require an existing installation."));
        return result;
    }
    result.sdkInstallPath = root.relativeFilePath(pluginRoot);
    bool pluginOk = false;
    const QJsonObject plugin = jsonObject(
        QDir(pluginRoot).filePath(QStringLiteral("Harken.uplugin")), &pluginOk);
    result.integrationVersion = plugin.value(QStringLiteral("VersionName"))
                                    .toString();
    const auto installed = ProjectIntegrationExecutor::inspectUnreal(record, false);
    result.health = installed.valid ? EngineTargetHealth::Managed : EngineTargetHealth::Unmanaged;
    result.error = installed.error;
    result.canChangeSdk = installed.valid;
    result.canRemoveSdk = installed.valid;
    result.canMoveSdk = installed.valid && pluginOk;
    if (!installed.valid) {
        result.status = hubTranslate(QFileInfo::exists(QDir(pluginRoot).filePath(QStringLiteral(".harken-integration-state.json")))
            ? QStringLiteral("Installation record or path invalid") : QStringLiteral("Installation record missing"));
        result.remediation = hubTranslate(QStringLiteral("Restore this plugin's ownership record from its own backup and resolve any linked or unreadable paths, then check again. Without a valid record, use the original plugin installer to repair or remove the installation."));
    } else if (!pluginOk) {
        result.health = EngineTargetHealth::Incomplete;
        result.status = hubTranslate(QStringLiteral("SDK metadata needs repair"));
        result.error = hubTranslate(QStringLiteral("Harken.uplugin is missing or invalid, but the plugin ownership record is available."));
        result.remediation = hubTranslate(QStringLiteral("Use Change SDK with a complete Unreal plugin source to repair it, or Uninstall SDK to remove the recorded files after backup. Repair the plugin metadata before moving it."));
    }
    return result;
}

}

EngineTargetObservation EngineTargetProbe::observe(const EngineTargetRecord& record, bool verifyContent)
{
    auto result = observeLayout(record);
    if (!verifyContent || result.sdkInstallPath.isEmpty()) return result;
    result.sdkContentState = SdkContentState::Unverified;
    if (result.health != EngineTargetHealth::Managed && !result.canChangeSdk) {
        result.sdkContentDetails = result.error;
        return result;
    }
    bool modified = false;
    bool missing = false;
    if (record.engineKind == EngineKind::Unity) {
        UnityIntegrationRequest request;
        request.unityProjectPath = record.canonicalRoot;
        request.verifyInstalledOnly = true;
        const auto inspection = HubUnityIntegrationService::run(request);
        result.sdkContentState = inspection.success ? SdkContentState::MatchesInstallation
                                                   : SdkContentState::Unverified;
        modified = !inspection.modifiedFiles.isEmpty();
        missing = !inspection.missingFiles.isEmpty();
        QStringList details;
        for (const auto& step : inspection.steps) {
            if (step.status != UnityIntegrationStepStatus::Failed) continue;
            details << step.summary;
            if (!step.cause.isEmpty()) details << step.cause;
            details << step.details;
        }
        result.sdkContentDetails = details.join(QLatin1Char('\n'));
    } else {
        const auto inspection = ProjectIntegrationExecutor::inspectUnreal(record);
        result.sdkContentState = inspection.valid && inspection.unreadableManagedFiles.isEmpty()
            ? SdkContentState::MatchesInstallation : SdkContentState::Unverified;
        QStringList modifiedFiles = inspection.modifiedManagedFiles;
        for (const auto& path : inspection.missingManagedFiles) modifiedFiles.removeAll(path);
        for (const auto& path : inspection.unreadableManagedFiles) modifiedFiles.removeAll(path);
        modified = !modifiedFiles.isEmpty();
        missing = !inspection.missingManagedFiles.isEmpty();
        QStringList details;
        if (modified)
            details << hubTranslate(QStringLiteral("SDK files with contents different from installation:"))
                    << modifiedFiles.join(QLatin1Char('\n'));
        if (missing)
            details << hubTranslate(QStringLiteral("Missing SDK files:"))
                    << inspection.missingManagedFiles.join(QLatin1Char('\n'));
        if (!inspection.unreadableManagedFiles.isEmpty())
            details << hubTranslate(QStringLiteral("Unreadable SDK files:"))
                    << inspection.unreadableManagedFiles.join(QLatin1Char('\n'));
        if (!inspection.error.isEmpty()) details << inspection.error;
        result.sdkContentDetails = details.join(QLatin1Char('\n'));
    }
    if (modified && missing) result.sdkContentState = SdkContentState::ModifiedAndMissing;
    else if (modified) result.sdkContentState = SdkContentState::Modified;
    else if (missing) result.sdkContentState = SdkContentState::Missing;
    return result;
}

}
