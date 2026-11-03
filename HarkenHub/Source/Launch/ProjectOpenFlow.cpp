#include "Launch/ProjectOpenFlow.h"
#include "Launch/StudioLauncher.h"
#include "UI/DialogLayout.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectVersionSelection.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QListWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

}

bool ProjectOpenFlow::ensureRegistered(ProjectStore* store, const QString& path,
                                       QString* canonicalPath, QString* error)
{
    const ProjectProbeResult probe = ProjectProbe::probe(path);
    if (!probe.ok) {
        *error = probe.error;
        return false;
    }
    *canonicalPath = probe.canonicalPath;
    if (store->contains(probe.canonicalPath))
        return true;

    ProjectRecord record;
    record.projectId = probe.projectId;
    record.canonicalPath = probe.canonicalPath;
    record.displayName = probe.displayName;
    record.detectedSchema = probe.schemaVersion;
    if (!store->addProject(record)) {
        *error = store->lastError();
        return false;
    }
    return true;
}

bool ProjectOpenFlow::isVersionBlock(const QString& blockCode)
{
    return blockCode == QStringLiteral("VersionSelectionRequired")
        || blockCode == QStringLiteral("VersionUnavailable")
        || blockCode == QStringLiteral("VersionNeedsRepair")
        || blockCode == QStringLiteral("ExecutableMissing");
}

ProjectOpenOutcome ProjectOpenFlow::open(
    ProjectStore* store, const QString& canonicalPath,
    const std::vector<InstalledVersion>& versions, QWidget* dialogParent,
    const QString& selectedBuildId)
{
    const auto records = store->listProjects();
    const auto record = std::find_if(records.begin(), records.end(),
        [&canonicalPath](const ProjectRecord& value) { return value.canonicalPath == canonicalPath; });
    if (record == records.end())
        return {ProjectOpenOutcome::Result::Failed, {}, hubText("The project file could not be found.")};
    const auto probe = ProjectProbe::probe(canonicalPath);
    auto selection = ProjectVersionSelection::resolve(*record, probe, versions, selectedBuildId);
    if (!selection.errorCode.isEmpty()) {
        if (versions.empty()) {
            return {ProjectOpenOutcome::Result::Blocked, selection.errorCode,
                    hubText("Install a Studio version from Versions before opening this project.")};
        }
        QStringList choices;
        for (const auto& version : versions)
            choices.append(QStringLiteral("%1 [%2] — %3")
                .arg(version.semanticVersion, version.buildId,
                     QDir::toNativeSeparators(version.executablePath)));
        QDialog dialog(dialogParent);
        dialog.setWindowTitle(hubText("Select Version"));
        configureDialog(&dialog, QSize(640, 360));
        auto* layout = new QVBoxLayout(&dialog);
        auto* reason = new DialogTextView(&dialog);
        reason->setText(selection.reason);
        layout->addWidget(reason);
        auto* list = new QListWidget(&dialog);
        list->addItems(choices);
        list->setCurrentRow(0);
        layout->addWidget(list, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return {ProjectOpenOutcome::Result::Declined, {}, {}};
        selection.buildId = versions[static_cast<size_t>(list->currentRow())].buildId;
        store->setPreferredBuild(canonicalPath, selection.buildId);
    }
    const auto decision = LaunchPreflight::evaluate(*record, versions, InstallIndex::snapshot(),
        selection.buildId);
    store->setLastPreflight(canonicalPath, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (decision.verdict != LaunchVerdict::Ready)
        return {ProjectOpenOutcome::Result::Blocked, decision.blockCode, decision.reason};
    StudioLaunchSpec spec;
    spec.installPath = decision.version.installPath;
    spec.executablePath = decision.version.executablePath;
    spec.projectPath = canonicalPath;
    QString error;
    if (!StudioLauncher::start(spec, &error))
        return {ProjectOpenOutcome::Result::Failed, {}, error};
    return {ProjectOpenOutcome::Result::Started, {}, {}};
}

}
