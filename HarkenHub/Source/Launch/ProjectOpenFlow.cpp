#include "Launch/ProjectOpenFlow.h"

#include "Launch/LaunchPreflight.h"
#include "Launch/ProjectOpenAnalyzer.h"
#include "Launch/StudioLaunchMonitor.h"
#include "Launch/StudioLauncher.h"
#include "Maintenance/BuildPinRegistry.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectVersionSelection.h"
#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectStore.h"
#include "Transactions/InstallIndex.h"

#include <QDateTime>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QInputDialog>
#include <QPointer>
#include <QProgressDialog>
#include <QThreadPool>
#include <QSet>
#include <QFileInfo>
#include <QMessageBox>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QWidget>
#include <algorithm>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

QSet<QString> activeProjects;
constexpr int kHandshakeTimeoutMs = 15000;
constexpr int kLaunchTimeoutMs = 120000;

QString analysisFailureText(const QString& code)
{
    if (code == QStringLiteral("ANALYZER_START_FAILED"))
        return hubText("The selected Harken Studio could not start the project safety check.");
    if (code == QStringLiteral("ANALYZER_TIMEOUT"))
        return hubText("The selected Harken Studio did not finish the project safety check in time.");
    if (code == QStringLiteral("ANALYZER_CRASHED"))
        return hubText("The selected Harken Studio closed while checking the project.");
    if (code.startsWith(QStringLiteral("ANALYSIS_"))
        || code == QStringLiteral("ANALYZER_EXIT_MISMATCH"))
        return hubText("The selected Harken Studio returned an invalid project safety report.");
    if (code == QStringLiteral("PROJECT_REQUIRES_NEWER_STUDIO"))
        return hubText("This project requires a newer Studio. Choose or install a newer version.");
    if (code == QStringLiteral("PROJECT_MIGRATION_MISSING"))
        return hubText("The selected Harken Studio does not contain a complete migration path for this project.");
    if (code == QStringLiteral("PROJECT_UPGRADE_SPACE_INSUFFICIENT"))
        return hubText("There is not enough free space to upgrade and protect this project safely.");
    if (code == QStringLiteral("PROJECT_VOLUME_UNSUPPORTED"))
        return hubText("This project is stored on a volume that cannot be upgraded safely.");
    if (code == QStringLiteral("PROJECT_RECOVERY_VOLUME_UNAVAILABLE"))
        return hubText("A safe recovery location is not available for this project upgrade.");
    if (code == QStringLiteral("PROJECT_UPGRADE_RECOVERY_REQUIRED")
        || code == QStringLiteral("PROJECT_UPGRADE_INCOMPLETE_ELSEWHERE"))
        return hubText("This project has an unfinished upgrade that must be recovered before it can open.");
    if (code == QStringLiteral("PROJECT_IN_USE"))
        return hubText("This project is currently in use by another process.");
    if (code == QStringLiteral("PROJECT_INSTANCE_UNAVAILABLE"))
        return hubText("Harken Studio could not contact or prepare the project window. Try opening the project again.");
    if (code == QStringLiteral("PROJECT_FORWARD_UNCONFIRMED"))
        return hubText("The existing project window was activated, but the launch request was not confirmed.");
    if (code == QStringLiteral("PROJECT_FILE_DAMAGED")
        || code == QStringLiteral("PROJECT_HAS_NO_FORMAL_BASELINE"))
        return hubText("The project data is incomplete or damaged and cannot be opened safely.");
    if (code == QStringLiteral("PROJECT_CHANGED_DURING_UPGRADE"))
        return hubText("The project changed after the safety check. Click Open Project to check it again.");
    return hubText("Harken Studio could not complete the project safety check.");
}

bool confirmUpgrade(QWidget* parent, const ProjectOpenAnalysis& analysis)
{
    const QString version = analysis.studioVersion.isEmpty()
        ? hubText("the selected version") : analysis.studioVersion;
    QString protection;
    if (analysis.protection == QStringLiteral("recoverableInPlace")) {
        protection = hubText("Recovery data will be stored at: %1")
                         .arg(QDir::toNativeSeparators(analysis.recoveryRoot));
    } else {
        protection = hubText("The files that may change will be protected by a recoverable transaction.");
    }
    const qint64 totalRequired = analysis.requiredProjectBytes
        + analysis.requiredRecoveryBytes;
    const QString text = hubText(
        "This project uses Schema %1.\n"
        "Harken Studio %2 uses Schema %3.\n\n"
        "Opening the project will upgrade its Schema. The migration has %4 step(s) and may change %5 file(s).\n"
        "%6\n"
        "Required free space: %7\n\n"
        "After the upgrade, older Harken Studio versions may no longer be able to open this project.")
                             .arg(analysis.currentSchema)
                             .arg(version)
                             .arg(analysis.targetSchema)
                             .arg(analysis.migrationStepCount)
                             .arg(analysis.affectedFileCount)
                             .arg(protection)
                             .arg(QLocale().formattedDataSize(totalRequired));
    QMessageBox box(QMessageBox::Warning, hubText("Project Upgrade Required"),
                    text, QMessageBox::NoButton, parent);
    QAbstractButton* upgrade = box.addButton(
        hubText("Upgrade and Open"), QMessageBox::AcceptRole);
    box.addButton(hubText("Cancel"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == upgrade;
}

class ProjectOpenSession : public QObject
{
public:
    ProjectOpenSession(ProjectStore* store, ProjectRecord project,
                       InstalledVersion version, QWidget* parent,
                       std::function<void(const ProjectOpenOutcome&)> completed)
        : QObject(parent ? static_cast<QObject*>(parent) : QCoreApplication::instance()), store_(store), project_(std::move(project)),
          version_(std::move(version)), parent_(parent), completed_(std::move(completed))
    {
        activeProjects.insert(project_.canonicalPath);
        successfulSchema_ = project_.detectedSchema;
        program_ = version_.executablePath.isEmpty()
            ? QDir(version_.installPath).filePath(studioBinaryRelativePath())
            : version_.executablePath;
        executableHash_ = PackageReader::fileSha256Hex(program_);
        requestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        progress_ = new QProgressDialog(hubText("Checking the project with Studio..."),
                                      hubText("Cancel"), 0, 0, parent);
        progress_->setWindowTitle(hubText("Open Project"));
        progress_->setWindowModality(Qt::NonModal);
        progress_->setMinimumDuration(0);
        progress_->setAutoClose(false);
        connect(progress_, &QProgressDialog::canceled, this, [this]() {
            if (!started_)
                finish(ProjectOpenOutcome::Result::Declined);
        });
        connect(&monitor_, &StudioLaunchMonitor::ready, this, [this]() { opened(); }, Qt::QueuedConnection);
        connect(&monitor_, &StudioLaunchMonitor::ready, &liveness_, &QTimer::stop);
        connect(&monitor_, &StudioLaunchMonitor::forwarded, this, [this]() {
            finish(ProjectOpenOutcome::Result::Activated);
        }, Qt::QueuedConnection);
        connect(&monitor_, &StudioLaunchMonitor::forwarded, &liveness_, &QTimer::stop);
        connect(&monitor_, &StudioLaunchMonitor::failed, &liveness_, &QTimer::stop);
        connect(&monitor_, &StudioLaunchMonitor::failed, this, [this](const QString& error) {
            if (finished_)
                return;
            if (monitor_.failureCode() == QStringLiteral("PROJECT_OPEN_CANCELLED")) {
                finish(ProjectOpenOutcome::Result::Declined);
            } else if (monitor_.failureCode().isEmpty() && launcher_.stillRunning()) {
                loseMonitoring(error);
            } else {
                fail(monitor_.failureCode().isEmpty() ? error : analysisFailureText(monitor_.failureCode()));
            }
        }, Qt::QueuedConnection);
        connect(&liveness_, &QTimer::timeout, this, [this]() {
            if (finished_)
                return;
            if (!launcher_.stillRunning()) {
                if (unmonitored_)
                    finish(ProjectOpenOutcome::Result::Unknown);
                else
                    fail(hubText("Studio exited before reporting that the project opened."));
            }
        });
    }

    ~ProjectOpenSession() override
    {
        activeProjects.remove(project_.canonicalPath);
        delete progress_.data();
    }

    void start()
    {
        if (version_.capabilitiesVersion != kStudioCapabilitiesVersion) {
            launch({});
            return;
        }
        const QPointer<ProjectOpenSession> self(this);
        const auto version = version_;
        const auto path = project_.canonicalPath;
        const auto request = requestId_;
        QThreadPool::globalInstance()->start([self, version, path, request]() {
            const auto analysis = ProjectOpenAnalyzer::run(version, path, request);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, analysis]() {
                if (self)
                    self->launch(analysis);
            }, Qt::QueuedConnection);
        });
    }

private:
    void launch(const ProjectOpenAnalysis& analysis)
    {
        if (finished_)
            return;
        LaunchIntent intent = LaunchIntent::DirectOpenOnly;
        QString approval;
        if (analysis.reportValid && analysis.ok) {
            successfulSchema_ = analysis.targetSchema;
            if (!project_.projectId.isEmpty()
                && QUuid(project_.projectId) != QUuid(analysis.projectId)) {
                fail(hubText("The project identity changed during the safety check. Add the project again before opening it."));
                return;
            }
            if (analysis.upgradeRequired) {
                progress_->hide();
                if (!confirmUpgrade(parent_, analysis)) {
                    finish(ProjectOpenOutcome::Result::Declined);
                    return;
                }
                intent = LaunchIntent::UpgradeApprovedByHub;
                approval = analysis.approvalDigest;
            }
        } else if (version_.capabilitiesVersion == kStudioCapabilitiesVersion) {
            qWarning().noquote() << analysis.errorCode << analysis.technicalDetails;
            progress_->setLabelText(analysisFailureText(analysis.errorCode)
                + QLatin1Char('\n') + hubText("Opening Studio to continue the check..."));
        }
        const auto decision = LaunchPreflight::evaluate(project_, {version_}, InstallIndex::snapshot(),
                                                        intent, version_.buildId);
        if (decision.verdict != LaunchVerdict::Ready) {
            fail(decision.reason);
            return;
        }
        if (executableHash_.isEmpty() || PackageReader::fileSha256Hex(program_) != executableHash_) {
            fail(hubText("Studio changed during the project check. Open the project again to check the new build."));
            return;
        }
        const bool monitored = version_.capabilitiesVersion == 1
            || version_.capabilitiesVersion == kStudioCapabilitiesVersion;
        if (monitored && !monitor_.listen()) {
            fail(hubText("Cannot open the launch progress channel."));
            return;
        }
        monitor_.setExpectedBuildId(version_.expectedBuildId());
        monitor_.setExpectedInstallPath(version_.installPath);
        monitor_.setExpectedOperation(QStringLiteral("openProject"));
        monitor_.setExpectedLaunchIntent(launchIntentName(intent));
        StudioLaunchSpec spec;
        spec.installPath = version_.installPath;
        spec.executablePath = program_;
        spec.projectPath = project_.canonicalPath;
        spec.serverName = monitor_.serverName();
        spec.sessionToken = monitor_.sessionToken();
        spec.expectedBuildId = version_.expectedBuildId();
        spec.intent = intent;
        spec.capabilitiesVersion = version_.capabilitiesVersion;
        if (intent == LaunchIntent::UpgradeApprovedByHub) {
            spec.projectOpenRequestId = requestId_;
            spec.projectOpenApproval = approval;
        }
        QString error;
        if (!launcher_.start(spec, &error)) {
            fail(error);
            return;
        }
        started_ = true;
        if (!monitored) {
            unmonitored_ = true;
            progress_->hide();
            liveness_.start(1000);
            return;
        }
        monitor_.setExpectedProcess(launcher_.pid(), program_);
        progress_->setLabelText(hubText("Studio is opening the project. Complete any confirmation in Studio."));
        progress_->setCancelButtonText(hubText("Hide"));
        progress_->show();
        liveness_.start(1000);
        QTimer::singleShot(kHandshakeTimeoutMs, this, [this]() {
            if (!monitor_.helloAccepted())
                loseMonitoring(hubText("Studio has not connected to Hub. Continue in Studio; its opening result is not yet known."));
        });
        QTimer::singleShot(kLaunchTimeoutMs, this, [this]() {
            if (!unmonitored_ && progress_)
                progress_->setLabelText(hubText("Studio is still opening the project. You can continue any confirmation there."));
        });
    }

    void loseMonitoring(const QString& reason)
    {
        if (finished_ || unmonitored_)
            return;
        unmonitored_ = true;
        monitor_.close();
        liveness_.start(1000);
        progress_->hide();
        QMessageBox::information(parent_, hubText("Open Project"), reason);
    }

    void opened()
    {
        if (finished_)
            return;
        const auto probe = ProjectProbe::probe(project_.canonicalPath);
        store_->recordSuccessfulOpen(project_.canonicalPath,
            !probe.ok || probe.projectId.isEmpty() ? project_.projectId : probe.projectId,
            !probe.ok || !probe.metadataWarning.isEmpty() ? project_.displayName : probe.displayName,
            probe.ok && probe.schemaVersion > 0 ? probe.schemaVersion : successfulSchema_, version_.buildId,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        QString error;
        if (!BuildPinRegistry::refreshFromProjects(*store_, &error))
            qWarning().noquote() << error;
        finish(ProjectOpenOutcome::Result::Launched);
    }

    void fail(const QString& reason)
    {
        if (finished_)
            return;
        const QPointer<QWidget> parent = parent_;
        finish(ProjectOpenOutcome::Result::Failed);
        QMessageBox::warning(parent, hubText("Open Project"), reason);
    }

    void finish(ProjectOpenOutcome::Result result)
    {
        if (finished_)
            return;
        finished_ = true;
        liveness_.stop();
        monitor_.close();
        progress_->hide();
        const auto completed = std::move(completed_);
        deleteLater();
        if (completed)
            completed({result, {}});
    }

    ProjectStore* store_;
    ProjectRecord project_;
    InstalledVersion version_;
    QPointer<QWidget> parent_;
    std::function<void(const ProjectOpenOutcome&)> completed_;
    QPointer<QProgressDialog> progress_;
    StudioLaunchMonitor monitor_;
    StudioLauncher launcher_;
    QTimer liveness_;
    QString program_;
    QString executableHash_;
    QString requestId_;
    int successfulSchema_ = 0;
    bool started_ = false;
    bool unmonitored_ = false;
    bool finished_ = false;
};

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
    const QString& selectedBuildId,
    std::function<void(const ProjectOpenOutcome&)> onFinished)
{
    if (canonicalPath.isEmpty() || activeProjects.contains(canonicalPath))
        return {ProjectOpenOutcome::Result::Busy, {}};
    const auto records = store->listProjects();
    const auto record = std::find_if(records.begin(), records.end(),
        [&canonicalPath](const ProjectRecord& value) { return value.canonicalPath == canonicalPath; });
    if (record == records.end())
        return {ProjectOpenOutcome::Result::Failed, {}};
    const auto probe = ProjectProbe::probe(canonicalPath);
    auto selection = ProjectVersionSelection::resolve(*record, probe, versions, selectedBuildId);
    if (!selection.errorCode.isEmpty()) {
        if (versions.empty()) {
            QMessageBox::information(dialogParent, hubText("Open Project"),
                                     hubText("Install a Studio version from Versions before opening this project."));
            return {ProjectOpenOutcome::Result::Blocked, selection.errorCode};
        }
        QStringList choices;
        for (const auto& version : versions)
            choices.append(QStringLiteral("%1 [%2] — %3")
                .arg(version.semanticVersion, version.buildId,
                     QDir::toNativeSeparators(version.executablePath)));
        bool accepted = false;
        const QString choice = QInputDialog::getItem(dialogParent, hubText("Select Version"),
            selection.reason, choices, 0, false, &accepted);
        if (!accepted)
            return {ProjectOpenOutcome::Result::Declined, {}};
        selection.buildId = versions[static_cast<size_t>(choices.indexOf(choice))].buildId;
        store->setPreferredBuild(canonicalPath, selection.buildId);
    }
    const auto decision = LaunchPreflight::evaluate(*record, versions, InstallIndex::snapshot(),
        LaunchIntent::DirectOpenOnly, selection.buildId);
    store->setLastPreflight(canonicalPath, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (decision.verdict != LaunchVerdict::Ready) {
        QMessageBox::warning(dialogParent, hubText("Open Project"), decision.reason);
        return {ProjectOpenOutcome::Result::Blocked, decision.blockCode};
    }
    auto* session = new ProjectOpenSession(store, *record, decision.version, dialogParent, std::move(onFinished));
    session->start();
    return {ProjectOpenOutcome::Result::Started, {}};
}

}
