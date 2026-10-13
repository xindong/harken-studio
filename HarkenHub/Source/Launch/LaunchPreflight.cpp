#include "Launch/LaunchPreflight.h"

#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectVersionSelection.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/IntegrityChecker.h"

#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QStringList>
#include <QUuid>

#include <algorithm>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

LaunchDecision blocked(const QString& code, const QString& reason)
{
    LaunchDecision decision;
    decision.verdict = LaunchVerdict::Blocked;
    decision.blockCode = code;
    decision.reason = reason;
    return decision;
}

QString healthStateFor(const InstallIndexSnapshot& index,
                       const QString& buildId)
{
    for (const auto& record : index.records) {
        if (record.buildId == buildId)
            return record.healthState;
    }
    return QString();
}

const InstalledVersionRecord* installRecordFor(
    const InstallIndexSnapshot& index, const QString& buildId)
{
    for (const auto& record : index.records)
        if (record.buildId == buildId)
            return &record;
    return nullptr;
}


}

QString launchIntentName(LaunchIntent intent)
{
    switch (intent) {
    case LaunchIntent::DirectOpenOnly:
        return QStringLiteral("DirectOpenOnly");
    case LaunchIntent::UpgradeApprovedByHub:
        return QStringLiteral("UpgradeApprovedByHub");
    }
    return QString();
}

LaunchDecision LaunchPreflight::evaluate(
    const ProjectRecord& project, const std::vector<InstalledVersion>& versions,
    const InstallIndexSnapshot& index, LaunchIntent intent,
    const QString& selectedBuildId)
{
    const QFileInfo projectInfo(project.canonicalPath);
    if (!projectInfo.exists() || !projectInfo.isFile())
        return blocked(QStringLiteral("ProjectMissing"),
                       hubText("Project file no longer exists: %1")
                           .arg(project.canonicalPath));
    {
        QFile file(project.canonicalPath);
        if (!file.open(QIODevice::ReadOnly))
            return blocked(QStringLiteral("ProjectUnreadable"),
                           hubText("Project file cannot be read"));
    }

    const ProjectProbeResult probe = ProjectProbe::probe(project.canonicalPath);
    if (!probe.ok)
        return blocked(QStringLiteral("ProjectUnparsable"), probe.error);
    if (!project.projectId.isEmpty() && !probe.projectId.isEmpty() &&
        QUuid(project.projectId) != QUuid(probe.projectId))
        return blocked(
            QStringLiteral("IdentityMismatch"),
            hubText("The project file identity has changed; it may "
                    "have been replaced by a different project"));

    const ProjectVersionSelection selection = ProjectVersionSelection::resolve(
        project, probe, versions, selectedBuildId);
    if (!selection.errorCode.isEmpty())
        return blocked(selection.errorCode, selection.reason);
    const auto target = std::find_if(versions.begin(), versions.end(),
        [&selection](const InstalledVersion& version) { return version.buildId == selection.buildId; });
    if (target == versions.end())
        return blocked(QStringLiteral("VersionUnavailable"), hubText("The selected Studio is not installed."));

    if (index.ok) {
        const QString health = healthStateFor(index, target->buildId);
        if (health == QStringLiteral("NeedsRepair"))
            return blocked(QStringLiteral("VersionNeedsRepair"),
                           hubText("The selected version needs repair "
                                   "before it can be used"));
    }

    if (!target->available)
        return blocked(QStringLiteral("VersionUnavailable"),
                       target->availabilityError.isEmpty()
                           ? hubText("The selected local build is unavailable")
                           : target->availabilityError);

    const QString executable = target->executablePath.isEmpty()
                                   ? target->installPath + QStringLiteral("/") +
                                         studioBinaryRelativePath()
                                   : target->executablePath;
    if (!QFileInfo::exists(executable))
        return blocked(QStringLiteral("ExecutableMissing"),
                       hubText("HarkenStudio executable is missing "
                               "from the selected version"));

    if (target->isLocalBuild()
        && (target->expectedBuildId().isEmpty()
            || target->executableFingerprint.isEmpty()
            || LocalBuildRegistry::fileFingerprint(executable) != target->executableFingerprint)) {
        QString error;
        if (!LocalBuildRegistry::requestRecheck(executable, target->executableFingerprint, &error))
            return blocked(QStringLiteral("VersionUnavailable"), error);
        return blocked(QStringLiteral("VersionUnavailable"),
                       hubText("This local build changed and must be checked again."));
    }

    if (!target->isLocalBuild()) {
        const InstalledVersionRecord* installRecord =
            index.ok ? installRecordFor(index, target->buildId) : nullptr;
        const IntegrityReport integrity = IntegrityChecker::check(
            target->installPath,
            installRecord ? installRecord->packageManifestHash : QString(),
            installRecord ? installRecord->capabilitiesHash : QString());
        if (!integrity.ok)
            return blocked(QStringLiteral("VersionNeedsRepair"),
                           hubText("The selected version failed its "
                                   "integrity check: %1")
                               .arg(integrity.error));
    }

    LaunchDecision decision;
    decision.version = *target;
    decision.intent = intent;
    decision.projectSchema = probe.schemaVersion;
    decision.verdict = LaunchVerdict::Ready;
    return decision;
}

}
