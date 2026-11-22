#include "Integrations/EngineTargetProbe.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Integrations/Unity/UnityIntegrationService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace harken::hub {
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
        result.remediation = hubTranslate(QStringLiteral("Restore the project directory or select its current location. No Integration Package files can be changed until the project is available."));
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
        auto discovery = harken::hub::unity::UnityInstallation::discover(record.canonicalRoot);
        if (discovery.installations.size() == 1)
            result.integrationInstallPath = root.relativeFilePath(discovery.installations.front().path);
        if (!discovery.errors.isEmpty() || discovery.installations.size() > 1) {
            result.health = EngineTargetHealth::Incomplete;
            result.error = discovery.errors.join(QLatin1Char('\n'));
            if (discovery.installations.size() > 1) {
                result.status = hubTranslate(QStringLiteral("Multiple Integration Package installations"));
                result.remediation = hubTranslate(QStringLiteral("Back up the project, identify the Integration Package copy used by the game, and remove the extra copies with their original integration tools. Check again when only one installation remains."));
                if (!result.error.isEmpty()) result.error += QLatin1Char('\n');
                result.error += hubTranslate(QStringLiteral("Multiple Integration Package installations were found:"));
                for (const auto& installation : discovery.installations)
                    result.error += QLatin1Char('\n') + installation.path;
            } else if (!discovery.integrationReferences.isEmpty()) {
                result.status = hubTranslate(QStringLiteral("Package Manager dependency"));
                result.remediation = hubTranslate(QStringLiteral("Manage this Integration Package dependency in Unity Package Manager. Before switching to a Hub installation, back up the project and remove the existing Integration Package dependency there, then check again."));
            } else {
                result.status = hubTranslate(QStringLiteral("Integration Package inspection blocked"));
                result.remediation = hubTranslate(QStringLiteral("Resolve the reported file access, package JSON or path errors, then check again. Hub must identify a single Integration Package location before changing its files."));
            }
            return result;
        }
        if (discovery.installations.isEmpty()) {
            result.health = EngineTargetHealth::NotInstalled;
            result.canChangeIntegration = true;
            result.remediation = hubTranslate(QStringLiteral("No Integration Package is installed. Use Install Integration Package and select a complete Integration Package source. Uninstalling requires an existing installation."));
            const QString candidate = record.integrationInstallPath.isEmpty()
                ? harken::hub::unity::UnityInstallation::defaultRelativePath() : record.integrationInstallPath;
            QString path;
            QString error;
            if (harken::hub::unity::UnityInstallation::resolveTarget(record.canonicalRoot, candidate, &path, &error)
                && QDir(path).exists() && !QDir(path).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
                result.integrationInstallPath = candidate;
                result.status = hubTranslate(QStringLiteral("Installation location is not empty"));
                result.remediation = hubTranslate(QStringLiteral("Choose an empty location or the folder of an existing Harken Integration Package. Hub replaces the whole installation directory, so it cannot install into a folder that contains other files."));
            }
            return result;
        }
        const QString packageRoot = discovery.installations.front().path;
        bool packageOk = false;
        const QJsonObject package = jsonObject(
            QDir(packageRoot).filePath(QStringLiteral("package.json")), &packageOk);
        result.integrationVersion = package.value(QStringLiteral("version")).toString();
        QString validatedPath;
        if (!harken::hub::unity::UnityInstallation::resolveTarget(record.canonicalRoot, result.integrationInstallPath,
                                                  &validatedPath, &result.error)
            || !harken::hub::unity::UnityInstallation::validateTree(packageRoot, &result.error)) {
            result.health = EngineTargetHealth::Incomplete;
            result.status = hubTranslate(QStringLiteral("Unsafe or unreadable Integration Package path"));
            result.remediation = hubTranslate(QStringLiteral("Resolve the reported path or permission problem. Integration Package folders must be real directories in the supported project locations, without symbolic links or junctions. Check again after correcting the path."));
            return result;
        }
        result.health = EngineTargetHealth::Managed;
        result.canChangeIntegration = true;
        result.canRemoveIntegration = discovery.integrationReferences.isEmpty();
        if (!result.canRemoveIntegration) {
            result.dependencyRestricted = true;
            result.status = hubTranslate(QStringLiteral("Installed; uninstall restricted by dependencies"));
            result.error = discovery.integrationReferences.join(QLatin1Char('\n'));
            result.remediation = hubTranslate(QStringLiteral("The Integration Package is installed and can still be changed. Before uninstalling, resolve the Integration Package dependencies in Unity Package Manager and package manifests so Unity cannot load another referenced copy."));
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
        result.error = hubTranslate(QStringLiteral("An unfinished Integration Package migration must be recovered before continuing."));
        result.remediation = hubTranslate(QStringLiteral("Use Recover Integration Package Migration to restore a consistent installation before changing, moving or uninstalling the Integration Package."));
        result.canMoveIntegration = true;
        return result;
    }
    QString pluginRoot;
    if (!ProjectIntegrationExecutor::locateUnrealInstallation(record, &pluginRoot, &result.error)) {
        result.health = EngineTargetHealth::Incomplete;
        result.status = hubTranslate(QStringLiteral("Integration Package inspection blocked"));
        result.remediation = hubTranslate(QStringLiteral("Resolve the reported plugin locations or path access errors, then check again. Hub must identify one Integration Package installation before changing its files."));
        return result;
    }
    if (pluginRoot.isEmpty()) {
        result.health = EngineTargetHealth::NotInstalled;
        result.canChangeIntegration = true;
        result.remediation = hubTranslate(QStringLiteral("No Integration Package is installed. Use Install Integration Package and select a complete Integration Package source. Moving and uninstalling require an existing installation."));
        return result;
    }
    result.integrationInstallPath = root.relativeFilePath(pluginRoot);
    bool pluginOk = false;
    const QJsonObject plugin = jsonObject(
        QDir(pluginRoot).filePath(QStringLiteral("Harken.uplugin")), &pluginOk);
    result.integrationVersion = plugin.value(QStringLiteral("VersionName"))
                                    .toString();
    const auto installed = ProjectIntegrationExecutor::inspectUnreal(record, false);
    result.health = installed.valid ? EngineTargetHealth::Managed : EngineTargetHealth::Unmanaged;
    result.error = installed.error;
    result.canChangeIntegration = installed.valid;
    result.canRemoveIntegration = installed.valid;
    result.canMoveIntegration = installed.valid && pluginOk;
    if (!installed.valid) {
        result.status = hubTranslate(QFileInfo::exists(QDir(pluginRoot).filePath(QStringLiteral(".harken-integration-state.json")))
            ? QStringLiteral("Installation record or path invalid") : QStringLiteral("Installation record missing"));
        result.remediation = hubTranslate(QStringLiteral("Restore this plugin's ownership record from its own backup and resolve any linked or unreadable paths, then check again. Without a valid record, use the original plugin installer to repair or remove the installation."));
    } else if (!pluginOk) {
        result.health = EngineTargetHealth::Incomplete;
        result.status = hubTranslate(QStringLiteral("Integration Package metadata needs repair"));
        result.error = hubTranslate(QStringLiteral("Harken.uplugin is missing or invalid, but the plugin ownership record is available."));
        result.remediation = hubTranslate(QStringLiteral("Use Change Integration Package with a complete Unreal plugin source to repair it, or Uninstall Integration Package to remove the recorded files after backup. Repair the plugin metadata before moving it."));
    }
    return result;
}

}

EngineTargetObservation EngineTargetProbe::observe(const EngineTargetRecord& record, bool verifyContent)
{
    auto result = observeLayout(record);
    if (record.engineKind == EngineKind::Unity) {
        result.canRestorePrevious = result.validTarget
            && !harken::hub::unity::UnityIntegrationService::previousVersionBackup(record.canonicalRoot).isEmpty();
        if (result.canRestorePrevious) {
            result.health = EngineTargetHealth::Incomplete;
            result.status = hubTranslate(QStringLiteral("Integration Package change unfinished"));
            result.remediation = hubTranslate(QStringLiteral("The last Integration Package change did not finish. Use Restore Previous Version to put back the version that was installed before, or install again."));
            result.canChangeIntegration = true;
        }
        return result;
    }
    if (!verifyContent || result.integrationInstallPath.isEmpty() || result.health == EngineTargetHealth::NotInstalled) return result;
    result.integrationContentState = IntegrationContentState::Unverified;
    if (result.health != EngineTargetHealth::Managed && !result.canChangeIntegration) {
        result.integrationContentDetails = result.error;
        return result;
    }
    bool modified = false;
    bool missing = false;
    const auto inspection = ProjectIntegrationExecutor::inspectUnreal(record);
    result.integrationContentState = inspection.valid && inspection.unreadableManagedFiles.isEmpty()
        ? IntegrationContentState::MatchesInstallation : IntegrationContentState::Unverified;
    QStringList modifiedFiles = inspection.modifiedManagedFiles;
    for (const auto& path : inspection.missingManagedFiles) modifiedFiles.removeAll(path);
    for (const auto& path : inspection.unreadableManagedFiles) modifiedFiles.removeAll(path);
    modified = !modifiedFiles.isEmpty();
    missing = !inspection.missingManagedFiles.isEmpty();
    QStringList details;
    if (modified)
        details << hubTranslate(QStringLiteral("Integration Package files with contents different from installation:"))
                << modifiedFiles.join(QLatin1Char('\n'));
    if (missing)
        details << hubTranslate(QStringLiteral("Missing Integration Package files:"))
                << inspection.missingManagedFiles.join(QLatin1Char('\n'));
    if (!inspection.unreadableManagedFiles.isEmpty())
        details << hubTranslate(QStringLiteral("Unreadable Integration Package files:"))
                << inspection.unreadableManagedFiles.join(QLatin1Char('\n'));
    if (!inspection.error.isEmpty()) details << inspection.error;
    result.integrationContentDetails = details.join(QLatin1Char('\n'));
    if (modified && missing) result.integrationContentState = IntegrationContentState::ModifiedAndMissing;
    else if (modified) result.integrationContentState = IntegrationContentState::Modified;
    else if (missing) result.integrationContentState = IntegrationContentState::Missing;
    return result;
}

}
